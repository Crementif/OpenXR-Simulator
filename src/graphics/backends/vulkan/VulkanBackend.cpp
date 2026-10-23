#include "Pch.h"
#include "graphics/SnapshotBudget.h"
#include "graphics/backends/vulkan/VulkanBackend.h"
#include "shaders/generated/CompositorSpirv.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

namespace simxr
{
XrResult VulkanGraphicsFailure(VkResult result)
{
	switch (result)
	{
	case VK_ERROR_OUT_OF_HOST_MEMORY:
	case VK_ERROR_OUT_OF_DEVICE_MEMORY:
		return XR_ERROR_OUT_OF_MEMORY;
	case VK_ERROR_FORMAT_NOT_SUPPORTED:
		return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	case VK_ERROR_OUT_OF_POOL_MEMORY:
	case VK_ERROR_TOO_MANY_OBJECTS:
		return XR_ERROR_LIMIT_REACHED;
	case VK_ERROR_EXTENSION_NOT_PRESENT:
	case VK_ERROR_FEATURE_NOT_PRESENT:
		return XR_ERROR_FEATURE_UNSUPPORTED;
	case VK_ERROR_DEVICE_LOST:
		return XR_ERROR_SESSION_LOST;
	default:
		return XR_ERROR_RUNTIME_FAILURE;
	}
}

namespace
{
struct Pipeline
{
	std::shared_ptr<VulkanDevice> device;
	VkRenderPass pass = VK_NULL_HANDLE;
	VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkSampler nearestSampler = VK_NULL_HANDLE;
	VkShaderModule vertex = VK_NULL_HANDLE;
	VkShaderModule pixel = VK_NULL_HANDLE;
	~Pipeline()
	{
		const auto native = device->binding.device;
		if (pipeline)
			vkDestroyPipeline(native, pipeline, nullptr);
		if (layout)
			vkDestroyPipelineLayout(native, layout, nullptr);
		if (descriptors)
			vkDestroyDescriptorSetLayout(native, descriptors, nullptr);
		if (nearestSampler)
			vkDestroySampler(native, nearestSampler, nullptr);
		if (sampler)
			vkDestroySampler(native, sampler, nullptr);
		if (pass)
			vkDestroyRenderPass(native, pass, nullptr);
		if (vertex)
			vkDestroyShaderModule(native, vertex, nullptr);
		if (pixel)
			vkDestroyShaderModule(native, pixel, nullptr);
	}
};

VkImageView View(const VulkanImage& image, std::uint32_t slice, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
{
	VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	info.image = image.image;
	info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	info.format = image.format;
	info.subresourceRange = {aspect, 0, image.mipLevels, slice, 1};
	VkImageView result;
	CheckVulkan(vkCreateImageView(image.device->binding.device, &info, nullptr, &result),
				"Create compositor image view");
	return result;
}

std::shared_ptr<Pipeline> CreatePipeline(std::shared_ptr<VulkanDevice> device, bool multisampledDepth = false)
{
	auto result = std::make_shared<Pipeline>();
	result->device = std::move(device);
	const auto native = result->device->binding.device;
	VkFormatProperties properties;
	vkGetPhysicalDeviceFormatProperties(result->device->binding.physicalDevice, VK_FORMAT_R8G8B8A8_SRGB, &properties);
	if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT))
		throw VulkanError(VK_ERROR_FORMAT_NOT_SUPPORTED, "Private sRGB color attachment blending unavailable");
	VkAttachmentDescription attachment{};
	attachment.format = VK_FORMAT_R8G8B8A8_SRGB;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
	const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color;
	const std::array dependencies{
		VkSubpassDependency{
			VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0},
		VkSubpassDependency{0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
							VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
							VK_ACCESS_MEMORY_READ_BIT, 0}};
	VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	pass.attachmentCount = 1;
	pass.pAttachments = &attachment;
	pass.subpassCount = 1;
	pass.pSubpasses = &subpass;
	pass.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
	pass.pDependencies = dependencies.data();
	CheckVulkan(vkCreateRenderPass(native, &pass, nullptr, &result->pass), "Create shared compositor render pass");
	const std::array bindings{
		VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
		VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
	VkDescriptorSetLayoutCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	descriptors.bindingCount = static_cast<std::uint32_t>(bindings.size());
	descriptors.pBindings = bindings.data();
	CheckVulkan(vkCreateDescriptorSetLayout(native, &descriptors, nullptr, &result->descriptors),
				"Create compositor descriptor layout");
	const VkPushConstantRange constants{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(LayerConstants)};
	VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	layout.setLayoutCount = 1;
	layout.pSetLayouts = &result->descriptors;
	layout.pushConstantRangeCount = 1;
	layout.pPushConstantRanges = &constants;
	CheckVulkan(vkCreatePipelineLayout(native, &layout, nullptr, &result->layout), "Create compositor pipeline layout");
	VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	module.codeSize = sizeof(shaders::vertex);
	module.pCode = shaders::vertex.data();
	CheckVulkan(vkCreateShaderModule(native, &module, nullptr, &result->vertex),
				"Create generated compositor vertex shader");
	const std::span<const std::uint32_t> pixel = multisampledDepth ? std::span<const std::uint32_t>{shaders::depth_ms} :
		std::span<const std::uint32_t>{shaders::pixel};
	module.codeSize = pixel.size_bytes();
	module.pCode = pixel.data();
	CheckVulkan(vkCreateShaderModule(native, &module, nullptr, &result->pixel),
				"Create generated compositor pixel shader");
	const std::array stages{
		VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
										VK_SHADER_STAGE_VERTEX_BIT, result->vertex, "VSMain", nullptr},
		VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
										VK_SHADER_STAGE_FRAGMENT_BIT, result->pixel, "PSMain", nullptr}};
	VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	viewport.viewportCount = viewport.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.lineWidth = 1;
	VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState blending{};
	blending.blendEnable = VK_TRUE;
	blending.srcColorBlendFactor = blending.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blending.dstColorBlendFactor = blending.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blending.colorBlendOp = blending.alphaBlendOp = VK_BLEND_OP_ADD;
	blending.colorWriteMask =
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	blend.attachmentCount = 1;
	blend.pAttachments = &blending;
	const std::array dynamics{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamics.size());
	dynamic.pDynamicStates = dynamics.data();
	VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pipeline.stageCount = static_cast<std::uint32_t>(stages.size());
	pipeline.pStages = stages.data();
	pipeline.pVertexInputState = &vertex;
	pipeline.pInputAssemblyState = &assembly;
	pipeline.pViewportState = &viewport;
	pipeline.pRasterizationState = &raster;
	pipeline.pMultisampleState = &multisample;
	pipeline.pColorBlendState = &blend;
	pipeline.pDynamicState = &dynamic;
	pipeline.layout = result->layout;
	pipeline.renderPass = result->pass;
	CheckVulkan(vkCreateGraphicsPipelines(native, VK_NULL_HANDLE, 1, &pipeline, nullptr, &result->pipeline),
				"Create shared compositor pipeline");
	VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sampler.minFilter = sampler.magFilter = VK_FILTER_LINEAR;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sampler.maxLod = VK_LOD_CLAMP_NONE;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	CheckVulkan(vkCreateSampler(native, &sampler, nullptr, &result->sampler), "Create compositor sampler");
	sampler.minFilter = sampler.magFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.maxLod = 0;
	CheckVulkan(vkCreateSampler(native, &sampler, nullptr, &result->nearestSampler), "Create depth preview sampler");
	return result;
}
} // namespace

struct VulkanBackend::State
{
	VulkanNativePreservation<State, 64>::Reservation preservation{
		"Sixty-four native Vulkan backend owners are retained"};
	struct Pending
	{
		std::shared_ptr<VulkanSubmission> submission;
		std::shared_ptr<VulkanFrame> frame;
	};
	struct Retirement
	{
		std::mutex mutex;
		std::vector<Pending> pending;
		void Poll()
		{
			std::array<Pending, 64> retired;
			std::size_t count = 0;
			{
				std::lock_guard lock(mutex);
				for (auto& entry : pending)
					if (entry.submission->NativeIdle())
						retired.at(count++) = std::move(entry);
				std::erase_if(pending,
							  [](const auto& entry)
							  {
								  return !entry.submission;
							  });
			}
			for (std::size_t index = 0; index < count; ++index)
				retired[index].submission->ReleaseSources();
		}
	};
	std::shared_ptr<VulkanDevice> device;
	std::shared_ptr<Pipeline> pipeline;
	std::shared_ptr<Pipeline> depthMultisamplePipeline;
	std::shared_ptr<VulkanExternalDevice> external;
	std::shared_ptr<Retirement> retirement = std::make_shared<Retirement>();
	State(std::size_t capacity, std::size_t liveCapacity)
		: reservations([capacity] { return capacity ? capacity : SnapshotBudget::Capacity(); },
					   [weak = std::weak_ptr<Retirement>(retirement)]
					   {
						   if (const auto retained = weak.lock())
							   retained->Poll();
					   }),
		  liveReservations(liveCapacity,
						   [weak = std::weak_ptr<Retirement>(retirement)]
						   {
							   if (const auto retained = weak.lock())
								   retained->Poll();
						   }),
		  historyReservations(3, [weak = std::weak_ptr<Retirement>(retirement)]
			{
				if (const auto retained = weak.lock())
					retained->Poll();
			})
	{
	}
	FramePool reservations;
	FramePool liveReservations;
	FramePool historyReservations;
	FramePool reprojectionReservations{32};
	bool closed = false;
	bool failed = false;
	std::vector<std::weak_ptr<VulkanSubmission>> submissions;
	std::vector<std::weak_ptr<VulkanImage>> images;
};

VulkanBackend::VulkanBackend(const VulkanBinding& binding, std::size_t frameCapacity,
							 std::span<const char* const> enabledExtensions, std::uint32_t instanceApiVersion,
							 std::size_t liveCapacity)
	: m_state(std::make_unique<State>(frameCapacity, liveCapacity))
{
	m_state->device = std::make_shared<VulkanDevice>(binding);
	const auto enabled = [&](const char* name)
	{
		return std::any_of(enabledExtensions.begin(), enabledExtensions.end(),
						   [&](const auto* value)
						   {
							   return value && std::strcmp(value, name) == 0;
						   });
	};
	if (enabled(VulkanExternalMemoryExtension) && enabled(VulkanExternalSemaphoreExtension))
		m_state->external =
			std::make_shared<VulkanExternalDevice>(m_state->device, enabledExtensions, instanceApiVersion);
	m_state->pipeline = CreatePipeline(m_state->device);
}

VulkanBackend::VulkanBackend(std::shared_ptr<VulkanDevice> device, std::size_t frameCapacity,
	std::span<const char* const> enabledExtensions, bool timelineEnabled)
	: m_state(std::make_unique<State>(frameCapacity, 1))
{
	if (!device || !device->HasNativeOwner() || !frameCapacity)
		throw std::invalid_argument("Independent composition requires an owned native device and capacity");
	m_state->device = std::move(device);
	if (!enabledExtensions.empty())
		m_state->external = std::make_shared<VulkanExternalDevice>(m_state->device, enabledExtensions,
			VK_API_VERSION_1_1, timelineEnabled);
	m_state->pipeline = CreatePipeline(m_state->device);
}

VulkanBackend::~VulkanBackend()
{
	if (m_state && XR_FAILED(Close()))
	{
		std::fputs("SimXR: Vulkan backend still has pending native work or retained readers; preserving owners\n",
				   stderr);
		m_state->preservation.Preserve(std::move(m_state));
	}
}

std::shared_ptr<VulkanDevice> VulkanBackend::Device() const
{
	return m_state->device;
}

bool VulkanBackend::Sharing() const
{
	return m_state->external != nullptr;
}

std::shared_ptr<VulkanImage> VulkanBackend::CreateImage(VkFormat format, VkExtent2D extent, std::uint32_t layers,
														VkImageUsageFlags usage, std::uint32_t mipLevels, VkSampleCountFlagBits samples)
{
	if (m_state->closed || m_state->failed || m_state->device->lost.load())
		throw std::logic_error("Vulkan backend closed");
	auto image = VulkanImage::Create(m_state->device, format, extent, layers, usage, mipLevels, samples);
	SnapshotBudget::ObserveImageBytes(image->allocationSize);
	m_state->images.push_back(image);
	return image;
}

FramePool VulkanBackend::Reservations() const
{
	return m_state->reservations;
}

FramePool VulkanBackend::HistoryReservations() const
{
	return m_state->historyReservations;
}

FramePool VulkanBackend::LiveReservations() const
{
	return m_state->liveReservations;
}

void VulkanBackend::RetireCompleted()
{
	m_state->retirement->Poll();
	std::erase_if(m_state->submissions,
				  [](const auto& entry)
				  {
					  return entry.expired();
				  });
	std::erase_if(m_state->images,
				  [](const auto& entry)
				  {
					  return entry.expired();
				  });
}

std::shared_ptr<VulkanSubmission> VulkanBackend::InitializeImage(std::shared_ptr<VulkanImage> image)
{
	RetireCompleted();
	if (m_state->closed || m_state->failed || m_state->device->lost.load() || m_state->submissions.size() >= 64)
		throw std::runtime_error("Vulkan initialization submission capacity unavailable");
	auto job = std::shared_ptr<VulkanSubmission>(new VulkanSubmission(m_state->device));
	job->m_resources->sources.push_back(std::move(image));
	const auto& source = *job->m_resources->sources.front();
	VulkanImageBarrier(job->m_resources->command, source, VK_IMAGE_LAYOUT_UNDEFINED, source.ApplicationLayout(), 0,
					   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
	CheckVulkan(vkEndCommandBuffer(job->m_resources->command), "End initial Vulkan image transition");
	m_state->submissions.push_back(job);
	{
		std::lock_guard lock(m_state->retirement->mutex);
		m_state->retirement->pending.push_back({job, {}});
		job->m_attempted.store(true);
	}
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &job->m_resources->command;
	const auto result = vkQueueSubmit(m_state->device->queue, 1, &submit, job->m_resources->fence);
	{
		std::lock_guard lock(m_state->retirement->mutex);
		job->m_submitResult.store(result);
		if (result != VK_SUCCESS)
		{
			auto expected = CompletionStatus::Pending;
			job->m_status.compare_exchange_strong(expected, CompletionStatus::Failed);
		}
		job->m_ready.store(true);
	}
	if (result != VK_SUCCESS)
	{
		m_state->failed = true;
		if (result == VK_ERROR_DEVICE_LOST)
			m_state->device->lost.store(true);
		throw VulkanError(result, "Submit initial Vulkan image transition");
	}
	return job;
}

XrResult VulkanBackend::Prepare(const CompositionPlan& plan, const VulkanImageResolver& resolve,
								std::shared_ptr<VulkanFrame>& output, std::shared_ptr<FrameReservation> reservation,
	std::uint32_t outputLayers)
{
	if (!resolve)
		return XR_ERROR_VALIDATION_FAILURE;
	return PrepareComposition(plan, resolve, {}, output, std::move(reservation), outputLayers);
}

XrResult VulkanBackend::PrepareRetained(const CompositionPlan& plan,
	std::span<const VulkanRetainedSource> sources, std::shared_ptr<VulkanFrame>& output)
{
	if (!m_state->device->HasNativeOwner() || sources.size() != plan.draws.size())
		return XR_ERROR_VALIDATION_FAILURE;
	return PrepareComposition(plan, {}, sources, output, {}, 2);
}

XrResult VulkanBackend::PrepareComposition(const CompositionPlan& plan, const VulkanImageResolver& resolve,
	std::span<const VulkanRetainedSource> retained, std::shared_ptr<VulkanFrame>& output,
	std::shared_ptr<FrameReservation> reservation, std::uint32_t outputLayers)
{
	RetireCompleted();
	if (m_state->closed || m_state->failed || m_state->device->lost.load())
		return XR_ERROR_SESSION_LOST;
	if (!plan.width || !plan.height || plan.draws.size() > 64 || !outputLayers || outputLayers > 2)
		return XR_ERROR_VALIDATION_FAILURE;
	if (reservation && !m_state->reservations.Owns(reservation) && !m_state->liveReservations.Owns(reservation) &&
		!m_state->historyReservations.Owns(reservation))
		return XR_ERROR_VALIDATION_FAILURE;
	if (reservation && reservation->Consumed())
		return XR_ERROR_CALL_ORDER_INVALID;
	if (m_state->submissions.size() >= 64)
		return XR_ERROR_LIMIT_REACHED;
	if (!reservation)
		reservation = m_state->reservations.Reserve();
	if (!reservation)
		return XR_ERROR_LIMIT_REACHED;
	const bool multisampledDepth = !plan.draws.empty() && plan.draws.front().constants.control[0] == 2 &&
		plan.draws.front().constants.control[2] != 0;
	if (multisampledDepth && std::any_of(plan.draws.begin(), plan.draws.end(), [](const auto& draw)
		{ return draw.constants.control[0] != 2 || !draw.constants.control[2]; }))
		return XR_ERROR_VALIDATION_FAILURE;
	if (multisampledDepth && !m_state->depthMultisamplePipeline)
		m_state->depthMultisamplePipeline = CreatePipeline(m_state->device, true);
	const auto& pipeline = multisampledDepth ? m_state->depthMultisamplePipeline : m_state->pipeline;
	auto job = std::shared_ptr<VulkanSubmission>(new VulkanSubmission(m_state->device));
	auto frame = std::shared_ptr<VulkanFrame>(new VulkanFrame(job, plan.views));
	m_state->submissions.push_back(job);
	auto& resources = *job->m_resources;
	resources.pipeline = pipeline;
	job->BeginTiming();
	if (m_state->external)
	{
		VulkanExternalImageSpec spec;
		spec.extent = {plan.width, plan.height, 1};
		spec.arrayLayers = outputLayers;
		resources.external = m_state->external->CreateExportable(spec);
		resources.output = resources.external->Image();
		SnapshotBudget::ObserveImageBytes(resources.external->Descriptor().allocationSize);
		m_state->images.push_back(resources.output);
		resources.ready = m_state->external->CreateExportableSemaphore();
	}
	else
		resources.output = CreateImage(VK_FORMAT_R8G8B8A8_SRGB, {plan.width, plan.height}, outputLayers,
									   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
										   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	const auto native = m_state->device->binding.device;
	for (std::uint32_t eye = 0; eye < outputLayers; ++eye)
	{
		resources.outputViews[eye] = View(*resources.output, eye);
		VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		framebuffer.renderPass = pipeline->pass;
		framebuffer.attachmentCount = 1;
		framebuffer.pAttachments = &resources.outputViews[eye];
		framebuffer.width = plan.width;
		framebuffer.height = plan.height;
		framebuffer.layers = 1;
		CheckVulkan(vkCreateFramebuffer(native, &framebuffer, nullptr, &resources.framebuffers[eye]),
					"Create private eye framebuffer");
	}
	std::vector<VkDescriptorSet> sets(plan.draws.size());
	if (!sets.empty())
	{
		const auto count = static_cast<std::uint32_t>(sets.size());
		const std::array sizes{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count},
							   VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, count}};
		VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		pool.maxSets = count;
		pool.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
		pool.pPoolSizes = sizes.data();
		CheckVulkan(vkCreateDescriptorPool(native, &pool, nullptr, &resources.descriptors),
					"Create bounded compositor descriptor storage");
		std::vector<VkDescriptorSetLayout> layouts(count, pipeline->descriptors);
		VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		allocate.descriptorPool = resources.descriptors;
		allocate.descriptorSetCount = count;
		allocate.pSetLayouts = layouts.data();
		CheckVulkan(vkAllocateDescriptorSets(native, &allocate, sets.data()), "Allocate compositor draw descriptors");
	}
	resources.sourceViews.reserve(plan.draws.size());
	resources.sources.reserve(plan.draws.size());
	resources.sourceBudgets.reserve(retained.size() + plan.draws.size());
	std::vector<std::pair<std::shared_ptr<VulkanImage>, std::shared_ptr<VulkanImage>>> resolved;
	const auto& sourcePool = m_state->reservations.Owns(reservation) ? m_state->reservations :
		m_state->historyReservations.Owns(reservation) ? m_state->historyReservations : m_state->liveReservations;
	for (std::size_t index = 0; index < plan.draws.size(); ++index)
	{
		const auto& draw = plan.draws[index];
		const bool depth = draw.constants.control[0] == 2;
		auto image = resolve ? resolve(draw.source.source) : retained[index].image;
		if (!resolve)
		{
			if (!retained[index].budget || !retained[index].budget->Consumed())
				return XR_ERROR_VALIDATION_FAILURE;
			resources.sourceBudgets.push_back(retained[index].budget);
		}
		if (!image || image->device != m_state->device || draw.source.arraySlice >= image->layers || draw.eye >= outputLayers ||
			!(image->usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
			(depth ? !(image->Aspect() & VK_IMAGE_ASPECT_DEPTH_BIT) : image->Aspect() != VK_IMAGE_ASPECT_COLOR_BIT))
			return XR_ERROR_LAYER_INVALID;
		if (depth && (image->samples != VK_SAMPLE_COUNT_1_BIT) != multisampledDepth)
			return XR_ERROR_VALIDATION_FAILURE;
		if (!depth && image->samples != VK_SAMPLE_COUNT_1_BIT)
		{
			if (!resolve || !(image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
				return XR_ERROR_FEATURE_UNSUPPORTED;
			const auto found = std::find_if(resolved.begin(), resolved.end(),
				[&](const auto& entry) { return entry.first == image; });
			if (found != resolved.end())
				image = found->second;
			else
			{
				auto budget = sourcePool.Reserve(false);
				if (!budget)
					return XR_ERROR_LIMIT_REACHED;
				auto destination = CreateImage(image->format, image->extent, image->layers,
					VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
				if (!sourcePool.Consume(budget))
					return XR_ERROR_LIMIT_REACHED;
				resources.sourceBudgets.push_back(std::move(budget));
				resources.sources.push_back(image);
				VulkanImageBarrier(resources.command, *image, image->ApplicationLayout(),
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
					VK_ACCESS_TRANSFER_READ_BIT);
				VulkanImageBarrier(resources.command, *destination, VK_IMAGE_LAYOUT_UNDEFINED,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
				VkImageResolve region{};
				region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, image->layers};
				region.extent = {image->extent.width, image->extent.height, 1};
				vkCmdResolveImage(resources.command, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					destination->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
				VulkanImageBarrier(resources.command, *image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					image->ApplicationLayout(), VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
				VulkanImageBarrier(resources.command, *destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					destination->ApplicationLayout(), VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
				resolved.emplace_back(image, destination);
				image = std::move(destination);
			}
		}
		VkFormatProperties properties;
		vkGetPhysicalDeviceFormatProperties(m_state->device->binding.physicalDevice, image->format, &properties);
		if (!(properties.optimalTilingFeatures & (depth ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
			: VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)))
			return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
		const auto view = View(*image, draw.source.arraySlice, depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
		resources.sourceViews.push_back(view);
		if (std::find(resources.sources.begin(), resources.sources.end(), image) == resources.sources.end())
			resources.sources.push_back(image);
		const VkDescriptorImageInfo sampled{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		const VkDescriptorImageInfo sampler{depth ? pipeline->nearestSampler : pipeline->sampler,
			VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
		const std::array writes{VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[index], 0, 0,
													 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &sampled, nullptr, nullptr},
								VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sets[index], 1, 0,
													 1, VK_DESCRIPTOR_TYPE_SAMPLER, &sampler, nullptr, nullptr}};
		vkUpdateDescriptorSets(native, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
	}
	for (const auto& image : resources.sources)
		VulkanImageBarrier(resources.command, *image, resolve ? image->ApplicationLayout() : VK_IMAGE_LAYOUT_GENERAL,
						   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						   VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
	for (std::uint32_t eye = 0; eye < outputLayers; ++eye)
	{
		const VkClearValue clear{{{0, 0, 0, 1}}};
		VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		pass.renderPass = pipeline->pass;
		pass.framebuffer = resources.framebuffers[eye];
		pass.renderArea.extent = {plan.width, plan.height};
		pass.clearValueCount = 1;
		pass.pClearValues = &clear;
		vkCmdBeginRenderPass(resources.command, &pass, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(resources.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
		const VkViewport viewport{0, 0, static_cast<float>(plan.width), static_cast<float>(plan.height), 0, 1};
		const VkRect2D scissor{{0, 0}, {plan.width, plan.height}};
		vkCmdSetViewport(resources.command, 0, 1, &viewport);
		vkCmdSetScissor(resources.command, 0, 1, &scissor);
		for (std::size_t index = 0; index < plan.draws.size(); ++index)
			if (plan.draws[index].eye == eye)
			{
				vkCmdBindDescriptorSets(resources.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout,
										0, 1, &sets[index], 0, nullptr);
				vkCmdPushConstants(resources.command, pipeline->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
								   sizeof(LayerConstants), &plan.draws[index].constants);
				vkCmdDraw(resources.command, 3, 1, 0, 0);
			}
		vkCmdEndRenderPass(resources.command);
	}
	for (const auto& image : resources.sources)
		VulkanImageBarrier(resources.command, *image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						   resolve ? image->ApplicationLayout() : VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
						   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
	if (resources.external)
	{
		VkImageMemoryBarrier release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		release.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		release.oldLayout = release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		release.srcQueueFamilyIndex = m_state->device->binding.queueFamily;
		release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
		release.image = resources.output->image;
		release.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, outputLayers};
		vkCmdPipelineBarrier(resources.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
							 0, 0, nullptr, 0, nullptr, 1, &release);
	}
	job->EndTiming();
	CheckVulkan(vkEndCommandBuffer(resources.command), "End shared compositor commands");
	const auto& pool = m_state->reservations.Owns(reservation) ? m_state->reservations :
		m_state->historyReservations.Owns(reservation) ? m_state->historyReservations : m_state->liveReservations;
	if (!pool.Consume(reservation))
		return XR_ERROR_CALL_ORDER_INVALID;
	resources.reservation = std::move(reservation);
	output = std::move(frame);
	return XR_SUCCESS;
}

XrResult VulkanBackend::PrepareDepthCapture(const LayerDepthDescription& depth, bool linear,
	const VulkanImageResolver& resolve, std::shared_ptr<VulkanFrame>& output,
	std::shared_ptr<FrameReservation> reservation)
{
	if (!resolve)
		return XR_ERROR_VALIDATION_FAILURE;
	const auto image = resolve(depth.image.source);
	if (!image || !(image->Aspect() & VK_IMAGE_ASPECT_DEPTH_BIT))
		return XR_ERROR_LAYER_INVALID;
	if (image->format != VK_FORMAT_D32_SFLOAT && image->format != VK_FORMAT_D16_UNORM &&
		image->format != VK_FORMAT_D24_UNORM_S8_UINT && image->format != VK_FORMAT_D32_SFLOAT_S8_UINT)
		return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	if (!(image->usage & VK_IMAGE_USAGE_SAMPLED_BIT))
		return XR_ERROR_FEATURE_UNSUPPORTED;
	CompositionPlan plan;
	const auto prepared = PrepareDepthComposition(depth, linear,
		{static_cast<std::int32_t>(image->extent.width), static_cast<std::int32_t>(image->extent.height)},
		image->samples != VK_SAMPLE_COUNT_1_BIT, plan);
	if (XR_FAILED(prepared))
		return prepared;
	return Prepare(plan, resolve, output, std::move(reservation), 1);
}

XrResult VulkanBackend::PrepareCapture(const LayerImageDescription& description, const VulkanImageResolver& resolve,
									   std::shared_ptr<VulkanFrame>& output,
									   std::shared_ptr<FrameReservation> reservation)
{
	return PrepareImageCopy(description, resolve, output, std::move(reservation), false);
}

XrResult VulkanBackend::PrepareRetainedLayer(const LayerImageDescription& description,
											 const VulkanImageResolver& resolve, std::shared_ptr<VulkanFrame>& output)
{
	RetireCompleted();
	auto reservation = m_state->reprojectionReservations.Reserve();
	if (!reservation)
		return XR_ERROR_LIMIT_REACHED;
	return PrepareImageCopy(description, resolve, output, std::move(reservation), true);
}

FramePool VulkanBackend::ReprojectionReservations() const
{
	return m_state->reprojectionReservations;
}

XrResult VulkanBackend::PrepareImageCopy(const LayerImageDescription& description, const VulkanImageResolver& resolve,
										 std::shared_ptr<VulkanFrame>& output,
										 std::shared_ptr<FrameReservation> reservation, bool retained)
{
	RetireCompleted();
	if (m_state->closed || m_state->failed || m_state->device->lost.load())
		return XR_ERROR_SESSION_LOST;
	if (!m_state->external)
		return XR_ERROR_FEATURE_UNSUPPORTED;
	if (!resolve || !description.source)
		return XR_ERROR_VALIDATION_FAILURE;
	const auto image = resolve(description.source);
	if (!image || image->device != m_state->device || description.arraySlice >= image->layers ||
		image->Aspect() != VK_IMAGE_ASPECT_COLOR_BIT || !(image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
		return XR_ERROR_LAYER_INVALID;
	if (!retained && image->format != VK_FORMAT_R8G8B8A8_SRGB && image->format != VK_FORMAT_R8G8B8A8_UNORM &&
		image->format != VK_FORMAT_B8G8R8A8_SRGB && image->format != VK_FORMAT_B8G8R8A8_UNORM)
		return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	const auto& rectangle = description.rectangle;
	if (rectangle.offset.x < 0 || rectangle.offset.y < 0 || rectangle.extent.width <= 0 ||
		rectangle.extent.height <= 0 ||
		static_cast<std::uint64_t>(rectangle.offset.x) + rectangle.extent.width > image->extent.width ||
		static_cast<std::uint64_t>(rectangle.offset.y) + rectangle.extent.height > image->extent.height)
		return XR_ERROR_SWAPCHAIN_RECT_INVALID;
	const auto& budget = retained ? m_state->reprojectionReservations : m_state->reservations;
	if (reservation && !budget.Owns(reservation))
		return XR_ERROR_VALIDATION_FAILURE;
	if (reservation && reservation->Consumed())
		return XR_ERROR_CALL_ORDER_INVALID;
	if (m_state->submissions.size() >= 64)
		return XR_ERROR_LIMIT_REACHED;
	if (!reservation)
		reservation = budget.Reserve();
	if (!reservation)
		return XR_ERROR_LIMIT_REACHED;
	auto job = std::shared_ptr<VulkanSubmission>(new VulkanSubmission(m_state->device));
	auto frame = std::shared_ptr<VulkanFrame>(new VulkanFrame(job));
	m_state->submissions.push_back(job);
	auto& resources = *job->m_resources;
	resources.sources.push_back(image);
	VulkanExternalImageSpec spec;
	spec.format = image->format;
	spec.extent = {static_cast<std::uint32_t>(rectangle.extent.width),
				   static_cast<std::uint32_t>(rectangle.extent.height), 1};
	spec.arrayLayers = 1;
	const bool fullMipChain = retained && image->mipLevels > 1;
	if (fullMipChain)
	{
		spec.extent = {image->extent.width, image->extent.height, 1};
		spec.mipLevels = image->mipLevels;
	}
	resources.external = m_state->external->CreateExportable(spec);
	resources.output = resources.external->Image();
		SnapshotBudget::ObserveImageBytes(resources.external->Descriptor().allocationSize);
	m_state->images.push_back(resources.output);
	resources.ready = m_state->external->CreateExportableSemaphore();
	resources.outputViews[0] = View(*resources.output, 0);
	VulkanImageBarrier(resources.command, *image, image->ApplicationLayout(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	VulkanImageBarrier(resources.command, *resources.output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
					   VK_ACCESS_TRANSFER_WRITE_BIT);
	VkImageCopy copy{};
	copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, description.arraySlice, 1};
	copy.srcOffset = fullMipChain ? VkOffset3D{} : VkOffset3D{rectangle.offset.x, rectangle.offset.y, 0};
	copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	for (std::uint32_t mip = 0; mip < spec.mipLevels; ++mip)
	{
		copy.srcSubresource.mipLevel = copy.dstSubresource.mipLevel = mip;
		copy.extent = {std::max(1U, spec.extent.width >> mip), std::max(1U, spec.extent.height >> mip), 1};
		if (image->samples == VK_SAMPLE_COUNT_1_BIT)
			vkCmdCopyImage(resources.command, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, resources.output->image,
				VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
		else
		{
			const VkImageResolve region{copy.srcSubresource, copy.srcOffset, copy.dstSubresource, copy.dstOffset, copy.extent};
			vkCmdResolveImage(resources.command, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				resources.output->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
		}
	}
	VulkanImageBarrier(resources.command, *image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image->ApplicationLayout(),
					   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
	VkImageMemoryBarrier release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	release.oldLayout = release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	release.srcQueueFamilyIndex = m_state->device->binding.queueFamily;
	release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	release.image = resources.output->image;
	release.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, spec.mipLevels, 0, 1};
	vkCmdPipelineBarrier(resources.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
						 0, nullptr, 0, nullptr, 1, &release);
	CheckVulkan(vkEndCommandBuffer(resources.command), "End exact layer/view copy commands");
	if (!budget.Consume(reservation))
		return XR_ERROR_CALL_ORDER_INVALID;
	resources.reservation = std::move(reservation);
	output = std::move(frame);
	return XR_SUCCESS;
}

#ifdef _WIN32
XrResult VulkanBackend::PrepareNativeCopy(std::shared_ptr<VulkanImage> source, VkSemaphore ready,
	std::uint64_t readyValue, std::shared_ptr<const void> owner, std::span<const FrameView> views,
	std::shared_ptr<VulkanFrame>& output, std::shared_ptr<FrameReservation> reservation)
{
	RetireCompleted();
	if (m_state->closed || m_state->failed || m_state->device->lost.load())
		return XR_ERROR_SESSION_LOST;
	if (!m_state->external)
		return XR_ERROR_FEATURE_UNSUPPORTED;
	if (!source || source->device != m_state->device || !ready || !owner ||
		(source->format != VK_FORMAT_R8G8B8A8_SRGB && source->format != VK_FORMAT_R8G8B8A8_UNORM &&
		 source->format != VK_FORMAT_B8G8R8A8_SRGB && source->format != VK_FORMAT_B8G8R8A8_UNORM &&
		 source->format != VK_FORMAT_R16G16B16A16_SFLOAT && source->format != VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
		 source->format != VK_FORMAT_R32G32B32A32_SFLOAT) ||
		!source->layers || source->layers > 2 || !source->mipLevels ||
		source->samples != VK_SAMPLE_COUNT_1_BIT || !(source->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
		return XR_ERROR_VALIDATION_FAILURE;
	if (m_state->submissions.size() >= 64)
		return XR_ERROR_LIMIT_REACHED;
	if (reservation && ((!m_state->reservations.Owns(reservation) && !m_state->liveReservations.Owns(reservation) &&
		!m_state->historyReservations.Owns(reservation) && !m_state->reprojectionReservations.Owns(reservation)) ||
		reservation->Consumed()))
		return XR_ERROR_VALIDATION_FAILURE;
	if (!reservation)
		reservation = m_state->reservations.Reserve();
	if (!reservation)
		return XR_ERROR_LIMIT_REACHED;
	auto job = std::shared_ptr<VulkanSubmission>(new VulkanSubmission(m_state->device));
	auto frame = std::shared_ptr<VulkanFrame>(new VulkanFrame(job, views));
	m_state->submissions.push_back(job);
	auto& resources = *job->m_resources;
	resources.sources.push_back(source);
	resources.nativeSource = std::move(owner);
	resources.nativeReady = ready;
	resources.nativeReadyValue = readyValue;
	VulkanExternalImageSpec spec;
	spec.format = source->format;
	spec.arrayLayers = source->layers;
	spec.mipLevels = source->mipLevels;
	spec.extent = {source->extent.width, source->extent.height, 1};
	resources.external = m_state->external->CreateExportable(spec);
	resources.output = resources.external->Image();
	SnapshotBudget::ObserveImageBytes(resources.external->Descriptor().allocationSize);
	m_state->images.push_back(resources.output);
	resources.ready = m_state->external->CreateExportableSemaphore();
	for (std::uint32_t eye = 0; eye < source->layers; ++eye)
		resources.outputViews[eye] = View(*resources.output, eye);
	VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	acquire.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	acquire.dstQueueFamilyIndex = m_state->device->binding.queueFamily;
	acquire.image = source->image;
	acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, source->mipLevels, 0, source->layers};
	vkCmdPipelineBarrier(resources.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &acquire);
	VulkanImageBarrier(resources.command, *resources.output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		0, VK_ACCESS_TRANSFER_WRITE_BIT);
	for (std::uint32_t mip = 0; mip < source->mipLevels; ++mip)
	{
		VkImageCopy copy{};
		copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, source->layers};
		copy.extent = {std::max(1u, spec.extent.width >> mip), std::max(1u, spec.extent.height >> mip), 1};
		vkCmdCopyImage(resources.command, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			resources.output->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
	}
	// Imported frames are immutable and never returned to their producer for reuse.
	VkImageMemoryBarrier release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	release.oldLayout = release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	release.srcQueueFamilyIndex = m_state->device->binding.queueFamily;
	release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	release.image = resources.output->image;
	release.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, source->mipLevels, 0, source->layers};
	vkCmdPipelineBarrier(resources.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0, 0, nullptr, 0, nullptr, 1, &release);
	CheckVulkan(vkEndCommandBuffer(resources.command), "End native compositor GPU copy");
	if (!(m_state->reservations.Owns(reservation) ? m_state->reservations :
		m_state->historyReservations.Owns(reservation) ? m_state->historyReservations :
		m_state->reprojectionReservations.Owns(reservation) ? m_state->reprojectionReservations : m_state->liveReservations).Consume(reservation))
		return XR_ERROR_CALL_ORDER_INVALID;
	resources.reservation = std::move(reservation);
	output = std::move(frame);
	return XR_SUCCESS;
}
#endif

XrResult VulkanBackend::Submit(const std::shared_ptr<VulkanFrame>& frame)
{
	return Submit(std::span<const std::shared_ptr<VulkanFrame>>(&frame, 1));
}

XrResult VulkanBackend::Submit(std::span<const std::shared_ptr<VulkanFrame>> frames)
{
	if (m_state->closed || m_state->failed || m_state->device->lost.load())
		return XR_ERROR_SESSION_LOST;
	if (frames.empty() || frames.size() > 64)
		return XR_ERROR_VALIDATION_FAILURE;
	std::array<VkCommandBuffer, 64> commands{};
	std::array<VkSemaphore, 64> signals{};
	std::size_t signalCount = 0;
#ifdef _WIN32
	std::array<VkSemaphore, 64> waits{};
	std::array<std::uint64_t, 64> waitValues{};
	std::array<std::uint64_t, 64> signalValues{};
	std::array<VkPipelineStageFlags, 64> waitStages{};
	std::size_t waitCount = 0;
#endif
	for (std::size_t index = 0; index < frames.size(); ++index)
	{
		const auto& frame = frames[index];
		if (!frame || frame->m_submission->m_resources->device != m_state->device)
			return XR_ERROR_SESSION_LOST;
		if (frame->Submitted())
			return XR_ERROR_CALL_ORDER_INVALID;
		for (std::size_t previous = 0; previous < index; ++previous)
			if (frames[previous] == frame)
				return XR_ERROR_VALIDATION_FAILURE;
		const auto& resources = *frame->m_submission->m_resources;
		commands[index] = resources.command;
#ifdef _WIN32
		if (resources.nativeReady)
		{
			waits[waitCount] = resources.nativeReady;
			waitValues[waitCount] = resources.nativeReadyValue;
			waitStages[waitCount++] = VK_PIPELINE_STAGE_TRANSFER_BIT;
		}
#endif
		if (resources.ready)
			signals[signalCount++] = resources.ready->Get();
	}
	const auto leader = frames.front()->m_submission;
	{
		std::lock_guard lock(m_state->retirement->mutex);
		if (m_state->retirement->pending.size() + frames.size() > 64)
			return XR_ERROR_LIMIT_REACHED;
		m_state->retirement->pending.reserve(m_state->retirement->pending.size() + frames.size());
		for (const auto& frame : frames)
		{
			auto job = frame->m_submission;
			if (job != leader)
				job->m_resources->completion = leader;
			m_state->retirement->pending.push_back({job, frame});
			job->m_attempted.store(true);
		}
	}
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
#ifdef _WIN32
	VkTimelineSemaphoreSubmitInfo nativeWait{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
	if (waitCount)
	{
		nativeWait.waitSemaphoreValueCount = static_cast<std::uint32_t>(waitCount);
		nativeWait.pWaitSemaphoreValues = waitValues.data();
		nativeWait.signalSemaphoreValueCount = static_cast<std::uint32_t>(signalCount);
		nativeWait.pSignalSemaphoreValues = signalValues.data();
		submit.pNext = &nativeWait;
		submit.waitSemaphoreCount = static_cast<std::uint32_t>(waitCount);
		submit.pWaitSemaphores = waits.data();
		submit.pWaitDstStageMask = waitStages.data();
	}
#endif
	submit.commandBufferCount = static_cast<std::uint32_t>(frames.size());
	submit.pCommandBuffers = commands.data();
	submit.signalSemaphoreCount = static_cast<std::uint32_t>(signalCount);
	submit.pSignalSemaphores = signals.data();
	const auto result = vkQueueSubmit(m_state->device->queue, 1, &submit, leader->m_resources->fence);
	{
		std::lock_guard lock(m_state->retirement->mutex);
		for (const auto& frame : frames)
		{
			auto& job = *frame->m_submission;
			job.m_submitResult.store(result);
			if (result != VK_SUCCESS)
			{
				auto expected = CompletionStatus::Pending;
				job.m_status.compare_exchange_strong(expected, CompletionStatus::Failed);
			}
			job.m_ready.store(true);
		}
	}
	if (result != VK_SUCCESS)
	{
		m_state->failed = true;
		if (result == VK_ERROR_DEVICE_LOST)
			m_state->device->lost.store(true);
		return XR_ERROR_RUNTIME_FAILURE;
	}
	return XR_SUCCESS;
}

VkResult VulkanBackend::Drain(std::uint64_t timeout)
{
	const auto deadline =
		std::chrono::steady_clock::now() + std::chrono::nanoseconds(std::min<std::uint64_t>(timeout, 5000000000ULL));
	std::array<State::Pending, 64> pending;
	std::size_t count = 0;
	{
		std::lock_guard lock(m_state->retirement->mutex);
		for (const auto& entry : m_state->retirement->pending)
			pending.at(count++) = entry;
	}
	for (std::size_t index = 0; index < count; ++index)
	{
		// A known rejected submission has no native work to wait for. Its
		// public completion remains Failed; unknown failures still retain use.
		if (pending[index].submission->PollCompletion() == CompletionStatus::Failed &&
			pending[index].submission->NativeIdle())
			continue;
		const auto now = std::chrono::steady_clock::now();
		const auto remaining =
			now < deadline ? std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count() : 0;
		const auto result = pending[index].submission->Wait(static_cast<std::uint64_t>(remaining));
		if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
			return result;
	}
	for (auto& entry : pending)
		entry = {};
	RetireCompleted();
	return m_state->device->lost.load() ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

XrResult VulkanBackend::Close()
{
	if (m_state->closed)
		return XR_SUCCESS;
	m_state->reservations.CloseAdmission();
	m_state->liveReservations.CloseAdmission();
	m_state->historyReservations.CloseAdmission();
	m_state->reprojectionReservations.CloseAdmission();
	const auto result = Drain(5000000000ULL);
	if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
		return XR_ERROR_RUNTIME_FAILURE;
	if (!m_state->submissions.empty() || !m_state->images.empty())
		return XR_ERROR_LIMIT_REACHED;
	m_state->closed = true;
	m_state->pipeline.reset();
	return XR_SUCCESS;
}
} // namespace simxr
