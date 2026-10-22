#include "graphics/backends/vulkan/VulkanReprojection.h"
#include "graphics/RotationalReprojection.h"

#include <algorithm>
#include <utility>

namespace simxr
{
namespace
{
struct RetainedImage
{
	LayerImageDescription original;
	std::shared_ptr<const VulkanInspectionFrame> output;
	XrExtent2Di extent{};
	bool fullMipChain = false;
};

bool SameImage(const LayerImageDescription& a, const LayerImageDescription& b)
{
	return a.source == b.source && a.arraySlice == b.arraySlice && a.rectangle.offset.x == b.rectangle.offset.x &&
		   a.rectangle.offset.y == b.rectangle.offset.y && a.rectangle.extent.width == b.rectangle.extent.width &&
		   a.rectangle.extent.height == b.rectangle.extent.height;
}

class VulkanRotationalSource final : public RotationalSource
{
  public:
	VulkanRotationalSource(RuntimeTime time, XrEnvironmentBlendMode blend, std::vector<LayerDescription> layers,
						   CompositionTarget target, PoseSample head, std::vector<RetainedImage> images)
		: m_layers(time, blend, std::move(layers)), m_target(target), m_head(head), m_images(std::move(images))
	{
	}
	CompletionStatus PollCompletion() const noexcept override
	{
		auto status = CompletionStatus::Complete;
		for (const auto& image : m_images)
		{
			if (!image.output)
				continue;
			const auto observed = image.output->PollCompletion();
			if (observed == CompletionStatus::Failed)
				return observed;
			if (observed == CompletionStatus::Pending)
				status = observed;
		}
		return status;
	}
	PoseSample SubmittedHead() const noexcept override
	{
		return m_head;
	}
	XrResult Reproject(const PoseSample& head, std::shared_ptr<const FrameOutput>& output) const override
	{
		const auto status = PollCompletion();
		if (status != CompletionStatus::Complete)
			return status == CompletionStatus::Failed ? XR_ERROR_RUNTIME_FAILURE : XR_TIMEOUT_EXPIRED;
		CompositionPlan plan;
		const auto result = PrepareRotationalComposition(
			m_layers, m_target, m_head, head,
			[this](XrSwapchain source) -> std::optional<XrExtent2Di>
			{
				const auto index = reinterpret_cast<std::uintptr_t>(source) - 1;
				if (index >= m_images.size())
					return {};
				const auto extent = m_images[index].extent;
				return XrExtent2Di{std::max(1, extent.width), std::max(1, extent.height)};
			},
			plan);
		if (XR_FAILED(result))
			return result;
		if (plan.draws.empty())
			return XR_ERROR_FEATURE_UNSUPPORTED;
		std::vector<std::shared_ptr<const VulkanInspectionFrame>> sources;
		sources.reserve(plan.draws.size());
		for (const auto& draw : plan.draws)
			sources.push_back(m_images.at(reinterpret_cast<std::uintptr_t>(draw.source.source) - 1).output);
		try
		{
			auto frame = VulkanInspectionFrame::Compose(plan, sources);
			if (!frame)
				return XR_TIMEOUT_EXPIRED;
			output = std::move(frame);
			return XR_SUCCESS;
		}
		catch (const VulkanError& error)
		{
			return VulkanGraphicsFailure(error.Result());
		}
	}

  private:
	const FrameLayers m_layers;
	const CompositionTarget m_target;
	const PoseSample m_head;
	const std::vector<RetainedImage> m_images;
};
} // namespace

XrResult VulkanReprojectionPreparation::Prepare(VulkanBackend& backend, VulkanInspection& inspection,
												const FrameLayers& layers, const CompositionTarget& target,
												const PoseSample& head, const VulkanImageResolver& resolve,
												std::size_t capacity,
												std::unique_ptr<VulkanReprojectionPreparation>& output,
												std::function<void(SharedImageKey)> deliveryRetired)
{
	if (!resolve)
		return XR_ERROR_VALIDATION_FAILURE;
	CompositionPlan checked;
	const auto valid = PrepareRotationalComposition(
		layers, target, head, head,
		[&resolve](XrSwapchain source) -> std::optional<XrExtent2Di>
		{
			const auto image = resolve(source);
			if (!image)
				return {};
			return XrExtent2Di{static_cast<std::int32_t>(image->extent.width),
							   static_cast<std::int32_t>(image->extent.height)};
		},
		checked);
	if (XR_FAILED(valid))
		return valid;
	auto prepared = std::unique_ptr<VulkanReprojectionPreparation>(new VulkanReprojectionPreparation);
	auto normalized = layers.layers;
	std::vector<RetainedImage> images;
	const auto retain = [&](LayerImageDescription& description) -> XrResult
	{
		auto found = std::find_if(images.begin(), images.end(),
								  [&](const auto& image)
								  {
									  return SameImage(image.original, description);
								  });
		std::size_t index = static_cast<std::size_t>(found - images.begin());
		if (found == images.end())
		{
			RetainedImage image{description, {}, description.rectangle.extent};
			if (description.rectangle.extent.width && description.rectangle.extent.height)
			{
				if (prepared->m_frames.size() >= capacity)
					return XR_ERROR_LIMIT_REACHED;
				std::shared_ptr<VulkanFrame> frame;
				const auto result = backend.PrepareRetainedLayer(description, resolve, frame);
				if (XR_FAILED(result))
					return result;
				image.extent = {static_cast<std::int32_t>(frame->Image().extent.width),
					static_cast<std::int32_t>(frame->Image().extent.height)};
				image.fullMipChain = frame->Image().mipLevels > 1;
				auto transfer = inspection.Prepare(frame, {}, {}, VulkanInspectionKind::Reprojection, deliveryRetired);
				image.output = transfer->Output();
				prepared->m_frames.push_back(std::move(frame));
				prepared->m_swapchains.push_back(description.source);
				prepared->m_transfers.push_back(std::move(transfer));
			}
			images.push_back(std::move(image));
		}
		// Mip chains keep original texel coordinates; base-only sources use exact crops.
		description.source = reinterpret_cast<XrSwapchain>(index + 1);
		description.arraySlice = 0;
		if (!images[index].fullMipChain)
			description.rectangle.offset = {0, 0};
		return XR_SUCCESS;
	};
	try
	{
		for (auto& layer : normalized)
		{
			if (auto* projection = std::get_if<ProjectionLayerDescription>(&layer))
			{
				for (auto& view : projection->views)
				{
					const auto result = retain(view.color);
					if (XR_FAILED(result))
						return result;
					view.depth.reset();
				}
			}
			else
			{
				const auto result = retain(std::get<QuadLayerDescription>(layer).color);
				if (XR_FAILED(result))
					return result;
			}
		}
	}
	catch (const VulkanError& error)
	{
		if (error.Result() == VK_ERROR_DEVICE_LOST)
			backend.Device()->lost.store(true);
		return VulkanGraphicsFailure(error.Result());
	}
	if (prepared->m_frames.empty())
		return XR_ERROR_FEATURE_UNSUPPORTED;
	prepared->m_source = std::make_shared<VulkanRotationalSource>(
		layers.displayTime, layers.blendMode, std::move(normalized), target, head, std::move(images));
	output = std::move(prepared);
	return XR_SUCCESS;
}

std::span<const std::shared_ptr<VulkanFrame>> VulkanReprojectionPreparation::Frames() const noexcept
{
	return m_frames;
}
std::span<const XrSwapchain> VulkanReprojectionPreparation::Swapchains() const noexcept
{
	return m_swapchains;
}
std::shared_ptr<const RotationalSource> VulkanReprojectionPreparation::Source() const noexcept
{
	return m_source;
}
void VulkanReprojectionPreparation::Commit() noexcept
{
	for (const auto& transfer : m_transfers)
		transfer->Commit();
}
} // namespace simxr
