#include "app/PreviewImages.h"
#include "app/UiRenderer.h"
#include "graphics/SnapshotBudget.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace simxr::app
{
namespace wire = control;
namespace tag = wire::tag;
using namespace panel;
using Work = PanelWork;
struct PreviewImages::Image
{
	std::uint64_t id = 0, reader = 0, delivery = 0, frame = 0, capture = 0, epoch = 0, session = 0, generation = 0;
	bool live = false, release = false, exporting = false;
	std::optional<std::uint64_t> imageTick;
	std::optional<wire::TargetKind> target;
	std::uint32_t layer = 0, view = 0;
	std::unique_ptr<PreviewTexture> texture;
};
PreviewImages::PreviewImages(UiRenderer& renderer, RuntimePanelRequests& requests)
	: m_renderer(renderer), m_requests(requests), m_history(renderer)
{
	m_images.reserve(SnapshotBudget::Capacity());
}
PreviewImages::~PreviewImages()
{
	std::uint64_t completed = 0;
	try
	{
		m_renderer.WaitIdle();
		completed = m_renderer.CompletedFrames();
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "Companion UI retirement: %s\n", error.what());
	}
	for (auto& image : m_images)
		image->texture->ObserveCompletion(completed);
}
PreviewImages::Image* PreviewImages::Find(std::uint64_t id) const
{
	for (const auto& image : m_images)
		if (image->id == id)
			return image.get();
	return nullptr;
}
void PreviewImages::SetScope(PanelScope scope)
{
	if (scope != m_scope && m_pending)
	{
		if (scope.epoch == m_scope.epoch)
			m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId,
							   U64(m_pending->message, tag::ImageReaderId));
		m_pending.reset();
	}
	if (scope.epoch != m_scope.epoch)
		for (auto& image : m_images)
		{
			image->reader = image->delivery = 0;
			if (!image->texture->Ready())
				image->release = true;
		}
	if (scope != m_scope)
		m_selectedCapture = 0;
	m_scope = scope;
}
bool PreviewImages::SelectCapture(PanelScope scope, std::uint64_t capture)
{
	m_selectedCapture = capture;
	m_live = false;
	// A newly selected snapshot must never display the previous frame while
	// its private image is still being requested.
	m_selectedImage = 0;
	for (const auto& image : m_images)
		if (!image->release && image->capture == capture && image->epoch == scope.epoch &&
			image->session == scope.session && image->generation == scope.generation)
		{
			m_selectedImage = image->id;
			return image->reader != 0;
		}
	return false;
}
void PreviewImages::SelectImage(std::uint64_t id)
{
	if (Find(id))
	{
		m_selectedImage = id;
		m_live = false;
	}
}
void PreviewImages::ClearSelection()
{
	m_selectedImage = 0;
}
void PreviewImages::ReleaseSelected()
{
	if (auto image = Find(m_selectedImage))
		image->release = true;
}
void PreviewImages::ShowLive(bool enabled) noexcept
{
	m_live = enabled;
}
bool PreviewImages::ShowingLive() const noexcept
{
	return m_live;
}
std::uint64_t PreviewImages::SelectedId() const noexcept
{
	return m_selectedImage;
}
std::string PreviewImages::TakeError()
{
	return std::exchange(m_error, {});
}
PreviewImageSummary PreviewImages::Summary(const Image& image) const
{
	return {image.id,
			image.reader,
			image.frame,
			image.capture,
			{image.epoch, image.session, image.generation},
			image.live,
			image.release,
			image.texture->Ready(),
			image.texture->Description(),
			image.texture->SliceCount(),
			image.target,
			image.layer,
			image.view,
			image.imageTick};
}
std::optional<PreviewImageSummary> PreviewImages::Selected() const
{
	if (const auto image = Find(m_selectedImage))
		return Summary(*image);
	return {};
}
std::vector<PreviewImageSummary> PreviewImages::Images() const
{
	std::vector<PreviewImageSummary> images;
	images.reserve(m_images.size());
	for (const auto& image : m_images)
		images.push_back(Summary(*image));
	return images;
}
std::size_t PreviewImages::ReadyCount() const
{
	return std::count_if(m_images.begin(), m_images.end(),
						 [](const auto& image)
						 {
							 return image->texture->Ready() && !image->release;
						 });
}
bool PreviewImages::HasPendingLive() const
{
	return std::any_of(m_images.begin(), m_images.end(),
					   [](const auto& image)
					   {
						   return image->live && !image->texture->Ready() && !image->release;
					   });
}
void PreviewImages::BeginFrame()
{
	m_history.BeginFrame();
	for (auto& image : m_images)
	{
		image->texture->BeginFrame();
		if (m_live && image->live && image->texture->Ready() && !image->release && image->epoch == m_scope.epoch &&
			image->session == m_scope.session && image->generation == m_scope.generation)
			m_selectedImage = image->id;
	}
}
void PreviewImages::DrawSelected(float width, std::optional<std::size_t> slice, ImVec4 crop)
{
	DrawImage(m_selectedImage, width, slice, crop);
}
void PreviewImages::DrawImage(std::uint64_t id, float width, std::optional<std::size_t> slice, ImVec4 crop)
{
	if (auto image = Find(id); image && !image->release && image->texture->Ready())
	{
		image->texture->Draw(width, slice, crop);
		if (image->live)
			m_history.Offer(*image->texture, static_cast<std::uint32_t>(slice.value_or(0)),
							{image->session, image->generation, image->frame}, image->epoch);
	}
}
void PreviewImages::RecordTransfers(VkCommandBuffer command)
{
	m_history.Record(command);
}
std::vector<VkSemaphore> PreviewImages::ReadyWaits() const
{
	std::vector<VkSemaphore> waits;
	for (const auto& image : m_images)
		if (const auto wait = image->texture->ReadyWait())
			waits.push_back(wait);
	return waits;
}
void PreviewImages::Submitted(std::uint64_t serial)
{
	for (auto& image : m_images)
		image->texture->Submitted(serial);
	m_history.Submitted(serial);
}
void PreviewImages::Retained(const wire::Message& message, bool preview)
{
	if (preview && !Boolean(message, tag::ImageAvailable))
	{
		for (auto& image : m_images)
			if (image->live)
				image->release = true;
		if (m_live)
			m_selectedImage = 0;
		return;
	}
	const auto reader = U64(message, tag::ImageReaderId), frame = Frame(message);
	const auto capture = U64(message, tag::CaptureId);
	const auto tickField = message.Find(tag::ImageTick);
	const auto imageTick = tickField ? tickField->AsU64() : std::nullopt;
	for (const auto& image : m_images)
		if (!image->release && image->epoch == m_scope.epoch && image->frame == frame && image->capture == capture &&
			image->live == preview && image->session == U64(message, tag::Session) &&
			image->generation == U64(message, tag::Generation) && (!preview || image->imageTick == imageTick))
		{
			if (!image->reader)
				image->reader = reader;
			else if (image->reader != reader)
				m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId, reader);
			if ((preview && m_live) || (!preview && m_selectedCapture == capture))
				m_selectedImage = image->id;
			return;
		}
	if (m_images.size() >= SnapshotBudget::Capacity())
	{
		if (m_pending)
			m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId,
							   U64(m_pending->message, tag::ImageReaderId));
		m_pending = PendingImage{message, preview};
		return;
	}
#if defined(__linux__) || defined(_WIN32)
	std::unique_ptr<Image> prepared;
	std::uint64_t localId = 0;
	try
	{
		prepared = std::make_unique<Image>();
		prepared->id = localId = m_nextImage++;
		prepared->reader = reader;
		const auto description = message.Find(tag::ImageDescriptor);
		if (!description)
			throw std::runtime_error("Runtime omitted the retained image description");
		const auto imageDescription = wire::DecodeImageDescription(*description);
		prepared->texture = std::make_unique<PreviewTexture>(m_renderer.DeviceAccess(), imageDescription);
		if (const auto target = message.Find(tag::Target))
		{
			const auto fields = Nested(target);
			prepared->target = static_cast<wire::TargetKind>(U32(fields, tag::CaptureKind));
			prepared->layer = U32(fields, tag::Layer);
			prepared->view = U32(fields, tag::View);
		}
		prepared->frame = frame;
		prepared->imageTick = imageTick;
		prepared->capture = capture;
		prepared->epoch = m_scope.epoch;
		prepared->live = preview;
		prepared->session = U64(message, tag::Session);
		prepared->generation = U64(message, tag::Generation);
		const auto identity = prepared->texture->Identity();
		auto fence = prepared->texture->ConsumerFence();
		m_images.push_back(std::move(prepared));
		std::vector<wire::Field> shareFields{wire::Field::U64(tag::ImageReaderId, reader, true),
											 wire::Field::Bytes(tag::DeviceUuid, identity.device, true),
											 wire::Field::Bytes(tag::DriverUuid, identity.driver, true),
											 wire::Field::U32(tag::HandleCount, 1)};
		if (imageDescription.completionKind != ImageCompletionKind::Fence)
			shareFields.push_back(wire::Field::U32(tag::CompletionKind,
												   static_cast<std::uint32_t>(imageDescription.completionKind), true));
		if (!m_requests.Send(wire::OpCode::ShareImage, Work::Share, std::move(shareFields), localId,
							 {std::move(fence)}))
		{
			Find(localId)->texture->AbandonUnsent();
			Find(localId)->release = true;
		}
	}
	catch (...)
	{
		if (auto admitted = Find(localId))
		{
			admitted->texture->AbandonUnsent();
			admitted->release = true;
		}
		else if (prepared && prepared->texture)
			prepared->texture->AbandonUnsent();
		m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId, reader);
		throw;
	}
#else
	m_error = "Native image transport is not implemented for this companion platform";
	m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId, reader);
#endif
}
#if defined(__linux__) || defined(_WIN32)
void PreviewImages::Shared(std::uint64_t id, const RuntimeReply& reply)
{
	if (auto image = Find(id))
	{
		image->delivery = U64(reply.packet.message, tag::DeliveryId);
		try
		{
			const auto description = reply.packet.message.Find(tag::ImageDescriptor);
			if (!description)
				throw std::runtime_error("Runtime omitted the native delivery description");
			image->texture->Import(wire::DecodeImageDescription(*description), reply.packet.handles);
		}
		catch (...)
		{
			image->release = true;
			throw;
		}
		const bool currentSource = image->epoch == m_scope.epoch && image->session == m_scope.session &&
								   image->generation == m_scope.generation;
		if (currentSource && ((!image->live && m_selectedCapture == image->capture) || (image->live && m_live)))
			m_selectedImage = image->id;
	}
}
#else
void PreviewImages::Shared(std::uint64_t, const RuntimeReply&)
{
	throw std::runtime_error("Native image delivery is unavailable on this companion platform");
}
#endif
void PreviewImages::ShareFailed(std::uint64_t id)
{
	if (auto image = Find(id))
		image->release = true;
}
void PreviewImages::ProtectExports(std::span<const std::uint64_t> images)
{
	for (auto& image : m_images)
		image->exporting = std::find(images.begin(), images.end(), image->id) != images.end();
}
void PreviewImages::Poll()
{
	std::size_t retained = std::count_if(m_images.begin(), m_images.end(),
										 [](const auto& image)
										 {
											 return !image->release;
										 });
	const auto capacity = SnapshotBudget::Capacity();
	const auto cacheLimit = capacity > 2 ? capacity - 2 : 1;
	for (auto& image : m_images)
		if (retained > cacheLimit && !image->exporting && image->id != m_selectedImage && !image->release && image->texture->Ready())
		{
			image->release = true;
			--retained;
		}
#if defined(__linux__) || defined(_WIN32)
	for (auto& image : m_images)
	{
		if (image->texture && !image->texture->Ready() && image->delivery)
		{
			const auto phase = image->texture->Poll();
			if (phase == CompletionStatus::Complete)
			{
				if (image->live)
					for (auto& previous : m_images)
						if (previous.get() != image.get() && previous->live && previous->texture->Ready())
							previous->release = true;
				std::printf("Companion retained GPU image frame=%llu slices=%u; normalReadback=0\n",
							static_cast<unsigned long long>(image->frame), image->texture->Description().arrayLayers);
				std::fflush(stdout);
			}
			else if (phase == CompletionStatus::Failed)
			{
				m_error = "Native image copy failed; actual GPU owners remain retained";
				image->release = true;
			}
		}
	}
#endif
	const auto completed = m_renderer.CompletedFrames();
	for (auto iterator = m_images.begin(); iterator != m_images.end();)
	{
		auto& image = **iterator;
		if (image.epoch == m_scope.epoch)
		{
			if ((image.texture->Ready() || image.release) && image.delivery)
			{
				m_requests.Release(wire::OpCode::ReleaseDelivery, tag::DeliveryId, image.delivery);
				image.delivery = 0;
			}
			if (!image.exporting && (image.release || (image.live && image.texture->Ready())) &&
				image.reader)
			{
				m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId, image.reader);
				image.reader = 0;
			}
		}
		if (image.exporting || !image.release || m_requests.Has(Work::Share, image.id))
		{
			++iterator;
			continue;
		}
		if (image.epoch == m_scope.epoch && image.reader)
		{
			++iterator;
			continue;
		}
		try
		{
			if (!image.texture->CanRetire(completed))
			{
				++iterator;
				continue;
			}
		}
		catch (const std::exception& error)
		{
			m_error = error.what();
			++iterator;
			continue;
		}
		if (m_selectedImage == image.id)
			m_selectedImage = 0;
		iterator = m_images.erase(iterator);
	}
	if (m_pending && m_images.size() < SnapshotBudget::Capacity())
	{
		auto pending = std::move(*m_pending);
		m_pending.reset();
		Retained(pending.message, pending.live);
	}
}
} // namespace simxr::app
