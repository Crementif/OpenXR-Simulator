#include "app/render/PreviewImages.h"
#include "app/UiPch.h"
#include "app/render/UiRenderer.h"
#include "graphics/SnapshotBudget.h"

namespace simxr::app
{
namespace wire = control;
namespace tag = wire::tag;
using namespace panel;
using Work = PanelWork;
struct PreviewImages::Image
{
	std::uint64_t id = 0, reader = 0, delivery = 0, frame = 0, capture = 0, epoch = 0, session = 0, generation = 0;
	bool live = false, release = false, exporting = false, thumbnail = false;
	std::optional<std::uint64_t> imageTick;
	std::optional<wire::TargetKind> target;
	std::uint32_t layer = 0, view = 0;
	std::unique_ptr<PreviewTexture> texture;
	std::shared_ptr<const wire::Message> state;
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
		if (!m_retiring)
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
void PreviewImages::BeginRetirement()
{
	if (m_retiring)
		return;
	m_retiring = true;
	if (m_pending)
	{
		m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId,
						   U64(m_pending->message, tag::ImageReaderId));
		m_pending.reset();
	}
	for (auto& image : m_images)
		image->release = true;
	m_selectedImage = 0;
	m_history.BeginRetirement();
}
bool PreviewImages::RetirementComplete() const
{
	return m_retiring && m_images.empty() && m_history.RetirementComplete() && !m_requests.Has(Work::Release) &&
		   !m_requests.Has(Work::Retain) && !m_requests.Has(Work::Preview) &&
		   !m_requests.Has(Work::PastPreview) && !m_requests.Has(Work::Thumbnail);
}
void PreviewImages::PreserveUnfinished() noexcept
{
	std::uint64_t completed = 0;
	try
	{
		completed = m_renderer.CompletedFrames();
	}
	catch (...)
	{
	}
	for (auto& image : m_images)
		image->texture->PreserveUnfinished(completed);
	m_images.clear();
	m_history.PreserveRetired();
}
void PreviewImages::SetCapacity(std::size_t capacity) noexcept
{
	if (m_capacity != capacity)
		m_memoryBlocked = false;
	m_capacity = capacity;
}
std::size_t PreviewImages::Capacity() const noexcept
{
	return m_capacity.value_or(SnapshotBudget::Capacity());
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
	m_selectedFrame = 0;
	m_live = false;
	// A newly selected snapshot must never display the previous frame while
	// its private image is still being requested.
	m_selectedImage = 0;
	for (const auto& image : m_images)
		if (!image->thumbnail && !image->release && image->capture == capture && image->epoch == scope.epoch &&
			image->session == scope.session && image->generation == scope.generation)
		{
			m_selectedImage = image->id;
			return image->reader != 0;
		}
	return false;
}
bool PreviewImages::SelectFrame(PanelScope scope, std::uint64_t frame)
{
	m_selectedCapture = 0;
	m_selectedFrame = frame;
	m_selectedImage = 0;
	m_live = false;
	for (const auto& image : m_images)
		if (!image->thumbnail && !image->release && image->frame == frame && image->epoch == scope.epoch &&
			image->session == scope.session && image->generation == scope.generation)
		{
			m_selectedImage = image->id;
			return image->reader != 0;
		}
	return false;
}
void PreviewImages::SelectImage(std::uint64_t id)
{
	if (const auto image = Find(id); image && !image->thumbnail)
	{
		m_selectedImage = id;
		m_live = false;
	}
}
void PreviewImages::ClearSelection()
{
	m_selectedImage = 0;
}
void PreviewImages::ReleaseLive()
{
	if (m_pending && m_pending->live)
	{
		m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId,
						   U64(m_pending->message, tag::ImageReaderId));
		m_pending.reset();
	}
	for (auto& image : m_images)
		if (image->live && !image->exporting)
		{
			image->release = true;
			if (m_selectedImage == image->id)
				m_selectedImage = 0;
		}
}
void PreviewImages::ReleaseInactive()
{
	ReleaseLive();
	if (m_pending && m_pending->thumbnail)
	{
		m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId,
						   U64(m_pending->message, tag::ImageReaderId));
		m_pending.reset();
	}
	for (auto& image : m_images)
		if (image->thumbnail && !image->exporting)
			image->release = true;
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
			image.imageTick,
			image.state};
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
		if (!image->thumbnail)
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
std::uint64_t PreviewImages::SnapshotChargeBytes() const
{
	std::uint64_t bytes = 0;
	for (const auto& image : m_images)
	{
		const auto charge = image->texture->SnapshotChargeBytes();
		if (charge > std::numeric_limits<std::uint64_t>::max() - bytes)
			return std::numeric_limits<std::uint64_t>::max();
		bytes += charge;
	}
	return bytes;
}
bool PreviewImages::HasPendingLive() const
{
	return std::any_of(m_images.begin(), m_images.end(),
					   [](const auto& image)
					   {
						   return image->live && !image->texture->Ready() && !image->release;
					   });
}
bool PreviewImages::HasPendingThumbnail() const
{
	return (m_pending && m_pending->thumbnail) || std::any_of(m_images.begin(), m_images.end(),
															  [](const auto& image)
															  {
																  return image->thumbnail && !image->release;
															  });
}
void PreviewImages::BeginFrame()
{
	m_history.BeginFrame();
	for (auto& image : m_images)
	{
		image->texture->BeginFrame();
		if (!image->live && !image->release && image->texture->Ready())
		{
			const FrameIdentity frame{image->session, image->generation, image->frame};
			const auto shot = m_history.Nearest(frame, image->epoch);
			if (shot && shot->frame == frame)
			{
				if (image->thumbnail)
					image->release = true;
			}
			else
				m_history.Offer(*image->texture, 0, frame, image->epoch);
		}
		if (m_live && image->live && image->texture->Ready() && !image->release && image->epoch == m_scope.epoch &&
			image->session == m_scope.session && image->generation == m_scope.generation)
			m_selectedImage = image->id;
	}
}
void PreviewImages::DrawSelected(float width, std::optional<std::size_t> slice, ImVec4 crop, bool correctProportions)
{
	DrawImage(m_selectedImage, width, slice, crop, correctProportions);
}
void PreviewImages::DrawImage(std::uint64_t id, float width, std::optional<std::size_t> slice, ImVec4 crop,
							  bool correctProportions)
{
	if (auto image = Find(id); image && !image->release && image->texture->Ready())
	{
		image->texture->Draw(width, slice, crop, correctProportions);
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
void PreviewImages::Retained(const wire::Message& message, bool preview, bool thumbnail)
{
	if (m_retiring)
	{
		m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId, U64(message, tag::ImageReaderId));
		return;
	}
	if (preview && !Boolean(message, tag::ImageAvailable))
	{
		if (m_pending && m_pending->live)
		{
			m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId,
							   U64(m_pending->message, tag::ImageReaderId));
			m_pending.reset();
		}
		for (auto& image : m_images)
			if (image->live)
				image->release = true;
		if (m_live)
			m_selectedImage = 0;
		return;
	}
	const auto reader = U64(message, tag::ImageReaderId), frame = Frame(message);
	std::shared_ptr<wire::Message> state;
	try
	{
		if (const auto* document = message.Find(tag::Document))
		{
			state = std::make_shared<wire::Message>();
			state->fields.push_back(*document);
		}
	}
	catch (...)
	{
		m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId, reader);
		throw;
	}
	const auto capture = U64(message, tag::CaptureId);
	if (Capacity() == 0)
	{
		m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId, reader);
		if (!std::exchange(m_memoryBlocked, true))
			m_error = "Snapshot memory is too small for this application's images.";
		return;
	}
	const auto tickField = message.Find(tag::ImageTick);
	const auto imageTick = tickField ? tickField->AsU64() : std::nullopt;
	for (const auto& image : m_images)
		if (!image->release && image->epoch == m_scope.epoch && image->frame == frame && image->capture == capture &&
			(image->live == preview || thumbnail || image->thumbnail) && image->session == U64(message, tag::Session) &&
			image->generation == U64(message, tag::Generation) && (!preview || image->imageTick == imageTick))
		{
			if (image->thumbnail && !thumbnail)
			{
				image->thumbnail = false;
				image->live = preview;
			}
			if (state)
				image->state = state;
			if (!image->reader)
				image->reader = reader;
			else if (image->reader != reader)
				m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId, reader);
			if (!thumbnail && ((preview && m_live) || (!preview && !m_live && m_selectedCapture == capture &&
													   (!m_selectedFrame || m_selectedFrame == frame))))
				m_selectedImage = image->id;
			return;
		}
	if (m_images.size() >= Capacity())
	{
		std::optional<PendingImage> prepared;
		try
		{
			prepared.emplace(PendingImage{message, preview, thumbnail});
		}
		catch (...)
		{
			m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId, reader);
			throw;
		}
		if (m_pending)
			m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId,
							   U64(m_pending->message, tag::ImageReaderId));
		m_pending = std::move(prepared);
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
		if (!PreviewTexture::CanReserveSnapshot(imageDescription.allocationSize))
		{
			PendingImage pending{message, preview, thumbnail};
			if (preview)
				ReleaseLive();
			for (auto& cached : m_images)
				if (!cached->exporting && !cached->release && cached->texture->Ready() &&
					(cached->id != m_selectedImage || (preview && m_live)))
				{
					cached->release = true;
					if (cached->id == m_selectedImage)
						m_selectedImage = 0;
				}
			if (m_pending)
				m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId,
								   U64(m_pending->message, tag::ImageReaderId));
			m_pending = std::move(pending);
			if (!std::exchange(m_memoryBlocked, true))
				m_error = "Snapshot memory is full. Increase the snapshot budget or release an image.";
			return;
		}
		m_memoryBlocked = false;
		prepared->texture = std::make_unique<PreviewTexture>(m_renderer.DeviceAccess(), imageDescription);
		if (const auto target = message.Find(tag::Target))
		{
			const auto fields = Nested(target);
			prepared->target = static_cast<wire::TargetKind>(U32(fields, tag::CaptureKind));
			prepared->layer = U32(fields, tag::Layer);
			prepared->view = U32(fields, tag::View);
		}
		prepared->frame = frame;
		prepared->state = std::move(state);
		prepared->imageTick = imageTick;
		prepared->capture = capture;
		prepared->epoch = m_scope.epoch;
		prepared->live = preview;
		prepared->thumbnail = thumbnail;
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
		if (image->release)
			return;
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
		if (currentSource && !image->thumbnail &&
			((!image->live && !m_live && m_selectedCapture == image->capture &&
			  (!m_selectedFrame || m_selectedFrame == image->frame)) ||
			 (image->live && m_live)))
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
	m_history.Poll();
	if (m_pending && Capacity() == 0)
	{
		m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId,
						   U64(m_pending->message, tag::ImageReaderId));
		m_pending.reset();
		if (!std::exchange(m_memoryBlocked, true))
			m_error = "Snapshot memory is too small for this application's images.";
	}
	std::size_t retained = std::count_if(m_images.begin(), m_images.end(),
										 [](const auto& image)
										 {
											 return !image->release;
										 });
	const auto capacity = Capacity();
	const auto cacheLimit = capacity > 2 ? capacity - 2 : 1;
	for (auto& image : m_images)
		if (retained > cacheLimit && !image->exporting && image->id != m_selectedImage && !image->release &&
			image->texture->Ready())
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
				m_error = "Could not copy the image. Its GPU memory is kept until it is safe to free.";
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
			if (!image.exporting && (image.release || (image.live && image.texture->Ready())) && image.reader)
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
	if (m_pending && m_images.size() < Capacity())
	{
		auto pending = std::move(*m_pending);
		m_pending.reset();
		Retained(pending.message, pending.live, pending.thumbnail);
	}
}
} // namespace simxr::app
