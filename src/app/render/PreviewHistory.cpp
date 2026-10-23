#include "app/render/PreviewHistory.h"
#include "Pch.h"
#include "app/render/UiRenderer.h"
#if defined(__linux__) || defined(_WIN32)
#include "graphics/backends/vulkan/VulkanDevice.h"
#include "graphics/backends/vulkan/VulkanResources.h"
#endif

#include <bit>
#include <cstdio>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <mutex>
#include <thread>

namespace simxr::app
{
static constexpr std::uint32_t ChunkLayers = 64, Width = 640;
static constexpr auto Interval = std::chrono::milliseconds(100);
// Frames between a requested frame and the kept image shown for it.
static constexpr std::uint64_t MaximumGap = 120;

#if defined(__linux__) || defined(_WIN32)
struct PreviewHistory::Resources
{
	struct Preserved;
	static Preserved& PreservedRings();
	std::unique_ptr<Resources> next;
	std::shared_ptr<VulkanDevice> device;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkExtent2D extent{};
	std::size_t capacity = 0;
	std::uint64_t budget = 0;
	std::vector<std::shared_ptr<VulkanImage>> chunks;
	std::size_t initialized = 0;
	std::shared_ptr<VulkanImage> display;
	bool displayInitialized = false;
	VkImageView view = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkDescriptorSet descriptor = VK_NULL_HANDLE;
	VkDescriptorPool thumbnailPool = VK_NULL_HANDLE;
	VkDescriptorSetLayout thumbnailLayout = VK_NULL_HANDLE;
	struct Thumbnail
	{
		VkImageView view = VK_NULL_HANDLE;
		VkDescriptorSet descriptor = VK_NULL_HANDLE;
	};
	std::vector<Thumbnail> thumbnails;
	std::uint64_t lastUse = 0;
	~Resources()
	{
		if (thumbnailPool)
			vkDestroyDescriptorPool(device->binding.device, thumbnailPool, nullptr);
		for (const auto& thumbnail : thumbnails)
			if (thumbnail.view)
				vkDestroyImageView(device->binding.device, thumbnail.view, nullptr);
		if (thumbnailLayout)
			vkDestroyDescriptorSetLayout(device->binding.device, thumbnailLayout, nullptr);
		if (descriptor)
			ImGui_ImplVulkan_RemoveTexture(descriptor);
		if (view)
			vkDestroyImageView(device->binding.device, view, nullptr);
		if (sampler)
			vkDestroySampler(device->binding.device, sampler, nullptr);
	}
	std::uint64_t Bytes() const
	{
		std::uint64_t bytes = display ? display->allocationSize : 0;
		for (const auto& chunk : chunks)
			bytes += chunk->allocationSize;
		return bytes;
	}
};
// Unknown GPU completion never justifies destruction: such rings stay for the
// process lifetime and count against later budgets.
struct PreviewHistory::Resources::Preserved
{
	std::mutex mutex;
	std::unique_ptr<PreviewHistory::Resources> rings;
	std::uint64_t bytes = 0;
};
PreviewHistory::Resources::Preserved& PreviewHistory::Resources::PreservedRings()
{
	static auto* preserved = new Preserved;
	return *preserved;
}
#else
struct PreviewHistory::Resources
{
	std::unique_ptr<Resources> next;
	std::uint64_t Bytes() const
	{
		return 0;
	}
};
#endif

PreviewHistory::PreviewHistory(UiRenderer& renderer, std::uint64_t budget) : m_renderer(renderer), m_budget(budget)
{
#if defined(__linux__) || defined(_WIN32)
	(void)Resources::PreservedRings();
#endif
}
PreviewHistory::~PreviewHistory()
{
	Retire();
#if defined(__linux__) || defined(_WIN32)
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	try
	{
		while (m_retired)
		{
			CollectRetired();
			if (!m_retired || std::chrono::steady_clock::now() >= deadline)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "Preview history retirement: %s\n", error.what());
	}
	if (m_retired)
	{
		std::fputs("Preview history completion unknown; preserving its images\n", stderr);
		auto& preserved = Resources::PreservedRings();
		std::lock_guard lock(preserved.mutex);
		while (m_retired)
		{
			auto ring = std::move(m_retired);
			m_retired = std::move(ring->next);
			preserved.bytes += ring->Bytes();
			ring->next = std::move(preserved.rings);
			preserved.rings = std::move(ring);
		}
	}
#endif
}
void PreviewHistory::Retire()
{
	if (m_resources)
	{
		m_resources->next = std::move(m_retired);
		m_retired = std::move(m_resources);
	}
}
void PreviewHistory::CollectRetired()
{
#if defined(__linux__) || defined(_WIN32)
	if (!m_retired)
		return;
	const auto completed = m_renderer.CompletedFrames();
	for (auto* link = &m_retired; *link;)
	{
		if ((*link)->lastUse <= completed)
		{
			auto retired = std::move(*link);
			*link = std::move(retired->next);
		}
		else
			link = &(*link)->next;
	}
#endif
}
bool PreviewHistory::Prepare(const control::ImageDescription& description)
{
#if defined(__linux__) || defined(_WIN32)
	if (!description.width || !description.height || description.width > INT32_MAX || description.height > INT32_MAX)
		throw std::invalid_argument("Preview history dimensions are invalid");
	const auto width = std::min(Width, description.width);
	const VkExtent2D extent{width, static_cast<std::uint32_t>(std::max<std::uint64_t>(
									   1, std::uint64_t(width) * description.height / description.width))};
	const auto format = static_cast<VkFormat>(description.format);
	if (m_resources && m_resources->format == format && m_resources->extent.width == extent.width &&
		m_resources->extent.height == extent.height)
		return true;
	// ImGui may already have borrowed this ring's descriptors for the current
	// frame. Finish that submission before replacing its resources or copy plan.
	if (m_display || !m_sampled.empty())
	{
		m_pendingDescription = description;
		return false;
	}
	Retire();
	CollectRetired();
	m_shots.clear();
	m_lastUse.clear();
	m_sampled.clear();
	m_next = 0;
	m_lastFrame = {};
	m_lastEpoch = 0;
	m_display = m_displayed = std::nullopt;
	const auto access = m_renderer.DeviceAccess();
	VkFormatProperties properties;
	vkGetPhysicalDeviceFormatProperties(access.physical, format, &properties);
	constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
											VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
											VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
	if ((properties.optimalTilingFeatures & needed) != needed)
	{
		m_error = "Preview history needs a blittable, filterable preview format";
		return false;
	}
	std::uint64_t preserved = PreviewTexture::PreservedBytes() + Bytes();
	{
		auto& rings = Resources::PreservedRings();
		std::lock_guard lock(rings.mutex);
		preserved += rings.bytes;
	}
	const auto available = m_budget > preserved ? m_budget - preserved : 0;
	auto resources = std::make_unique<Resources>();
	resources->device = std::make_shared<VulkanDevice>(
		VulkanBinding{access.instance, access.physical, access.device, access.queueFamily, 0}, access.owner);
	resources->format = format;
	resources->extent = extent;
	resources->budget = available;
	resources->display = VulkanImage::Create(resources->device, format, extent, 1,
											 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1,
											 VK_SAMPLE_COUNT_1_BIT, available);
	// Include format width and driver padding, not just the pixel count.
	const auto slot = resources->display->allocationSize;
	const auto capacity = (available - slot) / slot;
	if (!capacity)
	{
		m_error = "Preview history budget cannot hold a preview";
		return false;
	}
	resources->capacity = static_cast<std::size_t>(std::min<std::uint64_t>(capacity, 16384));
	VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	view.image = resources->display->image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = format;
	view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	CheckVulkan(vkCreateImageView(access.device, &view, nullptr, &resources->view), "create preview history view");
	VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	CheckVulkan(vkCreateSampler(access.device, &sampler, nullptr, &resources->sampler),
				"create preview history sampler");
	resources->descriptor = ImGui_ImplVulkan_AddTexture(resources->sampler, resources->view, VK_IMAGE_LAYOUT_GENERAL);
	if (!resources->descriptor)
		throw std::runtime_error("Preview history descriptor allocation failed");
	// A dedicated pool bounds the ring's descriptors without exhausting the UI pool.
	VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
										 nullptr};
	VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	layout.bindingCount = 1;
	layout.pBindings = &binding;
	CheckVulkan(vkCreateDescriptorSetLayout(access.device, &layout, nullptr, &resources->thumbnailLayout),
				"create thumbnail layout");
	VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
								  static_cast<std::uint32_t>(resources->capacity)};
	VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pool.maxSets = poolSize.descriptorCount;
	pool.poolSizeCount = 1;
	pool.pPoolSizes = &poolSize;
	CheckVulkan(vkCreateDescriptorPool(access.device, &pool, nullptr, &resources->thumbnailPool),
				"create thumbnail pool");
	resources->thumbnails.resize(resources->capacity);
	m_sampled.reserve(resources->capacity);
	m_shots.resize(resources->capacity);
	m_lastUse.resize(resources->capacity);
	m_resources = std::move(resources);
	return true;
#else
	(void)description;
	return false;
#endif
}
void PreviewHistory::Offer(PreviewTexture& source, std::uint32_t layer, FrameIdentity frame, std::uint64_t epoch)
{
#if defined(__linux__) || defined(_WIN32)
	const auto now = std::chrono::steady_clock::now();
	if (m_copy || (frame == m_lastFrame && epoch == m_lastEpoch) || !frame.frame || now - m_lastCapture < Interval ||
		!source.Ready() || layer >= source.Description().arrayLayers)
		return;
	try
	{
		if (!Prepare(source.Description()))
			return;
		auto& resources = *m_resources;
		if (!resources.capacity)
			return;
		const auto completed = m_renderer.CompletedFrames();
		const auto slot = m_next % resources.capacity;
		if (m_lastUse[slot] > completed || m_display == slot ||
			std::find(m_sampled.begin(), m_sampled.end(), slot) != m_sampled.end())
			return;
		if (slot / ChunkLayers >= resources.chunks.size())
		{
			auto layers = static_cast<std::uint32_t>(
				std::min<std::size_t>(ChunkLayers, resources.capacity - resources.chunks.size() * ChunkLayers));
			for (;;)
			{
				try
				{
					resources.chunks.push_back(VulkanImage::Create(
						resources.device, resources.format, resources.extent, layers,
						VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
						1, VK_SAMPLE_COUNT_1_BIT, resources.budget - resources.Bytes()));
					break;
				}
				catch (const std::length_error&)
				{
					if (--layers == 0)
					{
						resources.capacity = resources.chunks.size() * ChunkLayers;
						if (!resources.capacity)
							m_error = "Preview history budget cannot hold a preview";
						return;
					}
				}
			}
			if (layers < ChunkLayers)
				resources.capacity = (resources.chunks.size() - 1) * ChunkLayers + layers;
		}
		const auto image = source.UseForCopy();
		if (!image)
			return;
		m_copy = Copy{image, layer, {source.Description().width, source.Description().height}, slot, m_shots[slot]};
		m_shots[slot] = {slot, frame, epoch};
		if (m_displayed == slot)
			m_displayed.reset();
	}
	catch (const std::exception& error)
	{
		m_error = error.what();
	}
#else
	(void)source;
	(void)layer;
	(void)frame;
	(void)epoch;
#endif
}
std::optional<PreviewShot> PreviewHistory::Nearest(FrameIdentity frame, std::uint64_t epoch) const
{
	std::optional<PreviewShot> best;
	for (const auto& shot : m_shots)
		if (shot.frame.frame && shot.epoch == epoch && shot.frame.session == frame.session &&
			shot.frame.run == frame.run && shot.frame.frame <= frame.frame &&
			frame.frame - shot.frame.frame <= MaximumGap && (!best || shot.frame.frame > best->frame.frame))
			best = shot;
	return best;
}
float PreviewHistory::Aspect() const noexcept
{
#if defined(__linux__) || defined(_WIN32)
	if (m_resources)
		return float(m_resources->extent.height) / float(m_resources->extent.width);
#endif
	return 9.f / 16;
}
void PreviewHistory::Draw(const PreviewShot& shot, float width, ImVec4 crop)
{
#if defined(__linux__) || defined(_WIN32)
	if (!m_resources || !shot.frame.frame || shot.slot >= m_shots.size() || m_shots[shot.slot].frame != shot.frame ||
		m_shots[shot.slot].epoch != shot.epoch)
		return;
	m_display = shot.slot;
	ImGui::Image(std::bit_cast<ImTextureID>(m_resources->descriptor),
				 {width, width * Aspect() * (crop.w - crop.y) / (crop.z - crop.x)}, {crop.x, crop.y}, {crop.z, crop.w});
#else
	(void)shot;
	(void)width;
	(void)crop;
#endif
}
void PreviewHistory::DrawThumbnail(const PreviewShot& shot, ImVec2 minimum, ImVec2 maximum, ImVec4 crop)
{
#if defined(__linux__) || defined(_WIN32)
	if (!m_resources || !shot.frame.frame || shot.slot >= m_shots.size() || m_shots[shot.slot].frame != shot.frame ||
		m_shots[shot.slot].epoch != shot.epoch)
		return;
	auto& resources = *m_resources;
	auto& thumbnail = resources.thumbnails[shot.slot];
	if (!thumbnail.descriptor)
	{
		VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		view.image = resources.chunks[shot.slot / ChunkLayers]->image;
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = resources.format;
		view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, static_cast<std::uint32_t>(shot.slot % ChunkLayers),
								 1};
		if (!thumbnail.view)
			CheckVulkan(vkCreateImageView(resources.device->binding.device, &view, nullptr, &thumbnail.view),
						"create thumbnail view");
		VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		allocation.descriptorPool = resources.thumbnailPool;
		allocation.descriptorSetCount = 1;
		allocation.pSetLayouts = &resources.thumbnailLayout;
		CheckVulkan(vkAllocateDescriptorSets(resources.device->binding.device, &allocation, &thumbnail.descriptor),
					"allocate thumbnail descriptor");
		VkDescriptorImageInfo image{resources.sampler, thumbnail.view, VK_IMAGE_LAYOUT_GENERAL};
		VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		write.dstSet = thumbnail.descriptor;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = &image;
		vkUpdateDescriptorSets(resources.device->binding.device, 1, &write, 0, nullptr);
	}
	m_sampled.push_back(shot.slot);
	ImGui::GetWindowDrawList()->AddImage(std::bit_cast<ImTextureID>(thumbnail.descriptor), minimum, maximum,
										 {crop.x, crop.y}, {crop.z, crop.w});
#else
	(void)shot;
	(void)minimum;
	(void)maximum;
	(void)crop;
#endif
}
void PreviewHistory::Record(VkCommandBuffer command)
{
#if defined(__linux__) || defined(_WIN32)
	if (!m_resources)
		return;
	auto& resources = *m_resources;
	const bool show = m_display && m_display != m_displayed;
	if (!m_copy && !show)
		return;
	for (auto index = resources.initialized; index < resources.chunks.size(); ++index)
		VulkanImageBarrier(command, *resources.chunks[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
						   VK_ACCESS_TRANSFER_WRITE_BIT);
	if (!resources.displayInitialized)
	{
		VulkanImageBarrier(command, *resources.display, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
						   VK_ACCESS_TRANSFER_WRITE_BIT);
	}
	// Earlier frames on this queue may still sample the display image or copy slots.
	VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	before.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
	before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
						 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
	if (m_copy)
	{
		VkImageBlit blit{};
		blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, m_copy->layer, 1};
		blit.srcOffsets[1] = {static_cast<std::int32_t>(m_copy->extent.width),
							  static_cast<std::int32_t>(m_copy->extent.height), 1};
		blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<std::uint32_t>(m_copy->slot % ChunkLayers), 1};
		blit.dstOffsets[1] = {static_cast<std::int32_t>(resources.extent.width),
							  static_cast<std::int32_t>(resources.extent.height), 1};
		vkCmdBlitImage(command, m_copy->source, VK_IMAGE_LAYOUT_GENERAL,
					   resources.chunks[m_copy->slot / ChunkLayers]->image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
					   VK_FILTER_LINEAR);
	}
	if (show)
	{
		VkMemoryBarrier written{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		written.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		written.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &written, 0,
							 nullptr, 0, nullptr);
		VkImageCopy copy{};
		copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<std::uint32_t>(*m_display % ChunkLayers), 1};
		copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.extent = {resources.extent.width, resources.extent.height, 1};
		vkCmdCopyImage(command, resources.chunks[*m_display / ChunkLayers]->image, VK_IMAGE_LAYOUT_GENERAL,
					   resources.display->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
	}
	VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
						 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &after, 0,
						 nullptr, 0, nullptr);
	m_recorded = true;
#else
	(void)command;
#endif
}
void PreviewHistory::Submitted(std::uint64_t serial)
{
	if (!m_resources)
		return;
	for (const auto slot : m_sampled)
		m_lastUse[slot] = serial;
	m_sampled.clear();
	if (m_copy)
	{
		m_lastUse[m_copy->slot] = serial;
		m_lastFrame = m_shots[m_copy->slot].frame;
		m_lastEpoch = m_shots[m_copy->slot].epoch;
		m_lastCapture = std::chrono::steady_clock::now();
		++m_next;
		m_copy.reset();
	}
	if (m_display)
	{
		m_lastUse[*m_display] = serial;
		m_displayed = m_display;
		m_display.reset();
	}
#if defined(__linux__) || defined(_WIN32)
	m_resources->lastUse = serial;
	if (m_recorded)
	{
		m_resources->initialized = m_resources->chunks.size();
		m_resources->displayInitialized = true;
	}
#endif
}
void PreviewHistory::BeginFrame()
{
	CollectRetired();
	m_sampled.clear();
	// Work offered for a frame that was never submitted is dropped with it.
	if (m_copy)
		m_shots[m_copy->slot] = m_copy->previous;
	m_copy.reset();
	m_display.reset();
	m_recorded = false;
	if (m_pendingDescription)
	{
		const auto description = *m_pendingDescription;
		m_pendingDescription.reset();
		try
		{
			Prepare(description);
		}
		catch (const std::exception& error)
		{
			m_error = error.what();
		}
	}
}
std::uint64_t PreviewHistory::Bytes() const noexcept
{
	std::uint64_t bytes = m_resources ? m_resources->Bytes() : 0;
	for (const auto* ring = m_retired.get(); ring; ring = ring->next.get())
		bytes += ring->Bytes();
	return bytes;
}
} // namespace simxr::app
