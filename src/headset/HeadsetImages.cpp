#include "headset/HeadsetImages.h"
#include "Pch.h"
#include "headset/HeadsetGraphics.h"

namespace simxr::headset
{
HeadsetImages::HeadsetImages(HeadsetGraphics& graphics, HeadsetImageSource source, std::shared_ptr<VulkanDevice> device)
	: m_graphics(graphics),
	  m_device(device ? std::move(device)
					  : std::make_shared<VulkanDevice>(VulkanBinding{graphics.Instance(), graphics.PhysicalDevice(),
																	 graphics.Device(), graphics.QueueFamily(), 0})),
	  m_bridge(std::move(source))
{
	if (m_device->binding.device != graphics.Device() ||
		m_device->binding.physicalDevice != graphics.PhysicalDevice() ||
		m_device->binding.queueFamily != graphics.QueueFamily() || m_device->binding.queueIndex != 0)
		throw std::invalid_argument("Headset image device must use the physical rendering queue");
	m_frames.reserve(s_capacity);
}

HeadsetImages::~HeadsetImages()
{
	Close();
}

void HeadsetImages::Tick()
{
	if (m_closed)
		throw std::logic_error("Headset image stream is closed");
	const auto error = m_bridge.Error();
	if (!error.empty())
		throw std::runtime_error(error);
	RetireUnused();
	// Take every step already finished, so a reply or completed copy does not wait
	// another physical frame before the next request.
	for (unsigned step = 0; step < s_stepsPerTick && Step(); ++step)
	{}
}

bool HeadsetImages::Step()
{
	if (!m_pending)
	{
		if (m_frames.size() == s_capacity)
		{
			++m_timing.capacityWaits;
			return false;
		}
		m_pending = std::make_unique<control::VulkanImageConsumer>(m_device, m_graphics.Extensions(),
																   m_graphics.ApiVersion(), m_completionKind, true);
	}
	if (!m_requested)
	{
		if (!m_bridge.Request(m_pending->Identity(), m_pending->ConsumerFence(), m_imageTick, m_completionKind))
			return false;
		m_requested = true;
		++m_timing.requests;
		m_requestedAt = std::chrono::steady_clock::now();
		m_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	}
	if (!m_packet)
	{
		auto packet = m_bridge.Take();
		if (!packet)
		{
			if (std::chrono::steady_clock::now() >= m_deadline)
				throw std::runtime_error("Headset image delivery timed out");
			return false;
		}
		m_timing.maximumDelivery = std::max(
			m_timing.maximumDelivery,
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_requestedAt));
		if (packet->kind != HeadsetImageKind::Image)
		{
			if (packet->kind == HeadsetImageKind::CompletionChanged)
				m_completionKind = packet->description.completionKind;
			m_timing.unchanged += packet->kind == HeadsetImageKind::Unchanged;
			m_timing.clears += packet->kind == HeadsetImageKind::Clear;
			// A busy source or completion renegotiation says nothing about the current image.
			if (packet->kind == HeadsetImageKind::Unchanged || packet->kind == HeadsetImageKind::Clear)
				m_presentationAvailable = packet->kind == HeadsetImageKind::Unchanged &&
										  packet->sourceAge.has_value() && m_current &&
										  m_current->metadata.imageTick == packet->imageTick;
			// The worker never offered this completion handle to ShareImage.
			m_pending->AbandonUnsent();
			m_pending.reset();
			m_requested = false;
			if (packet->kind == HeadsetImageKind::Clear)
			{
				m_current.reset();
				m_imageTick = 0;
				RetireUnused();
			}
			return true;
		}
		m_packet.emplace(std::move(*packet));
		if (m_packet->description.views.size() != 2 || m_packet->description.arrayLayers != 2)
			throw std::runtime_error("Headset output requires two composed eyes and immutable pose/FOV metadata");
		m_copyStarted = std::chrono::steady_clock::now();
		m_pending->Import(m_packet->description, m_packet->handles);
		m_pending->Submit();
	}
	const auto status = m_pending->Poll();
	if (status == CompletionStatus::Pending)
	{
		if (std::chrono::steady_clock::now() >= m_deadline)
			throw std::runtime_error("Headset image copy timed out");
		return false;
	}
	if (status != CompletionStatus::Complete)
		throw std::runtime_error("Headset image copy failed");
	++m_timing.copies;
	m_timing.maximumCopyObservation = std::max(
		m_timing.maximumCopyObservation,
		std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_copyStarted));
	if (!m_bridge.Acknowledge(m_packet->delivery))
		throw std::runtime_error("Headset image delivery acknowledgement failed");
	auto frame = std::make_shared<HeadsetImageFrame>();
	frame->pixels = std::move(m_pending);
	frame->metadata = std::move(*m_packet);
	// Imported handles are no longer needed after the private copy completes.
	frame->metadata.handles.clear();
	m_imageTick = frame->metadata.imageTick;
	m_frames.push_back(frame);
	m_timing.maximumRetained = std::max(m_timing.maximumRetained, m_frames.size());
	m_current = std::move(frame);
	m_presentationAvailable = true;
	m_packet.reset();
	m_requested = false;
	RetireUnused();
	return true;
}

bool HeadsetImages::Close()
{
	if (m_closed)
		return m_retired;
	m_closed = true;
	m_bridge.Stop();
	bool idle = true;
	if (m_pending)
	{
		try
		{
			if (!m_requested)
				m_pending->AbandonUnsent();
			else
				m_pending->CancelUnused();
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!m_pending->NativeIdle() && std::chrono::steady_clock::now() < deadline)
			{
				m_pending->Poll();
				if (!m_pending->NativeIdle())
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		catch (...)
		{}
		idle = m_pending->NativeIdle();
	}
	m_current.reset();
	for (const auto& frame : m_frames)
		if (frame.use_count() != 1)
			idle = false;
	m_retired = idle && !m_device->lost;
	if (!m_retired)
		m_graphics.PreserveNativeResources();
	m_pending.reset();
	m_frames.clear();
	return m_retired;
}

std::shared_ptr<const HeadsetImageFrame> HeadsetImages::Current() const
{
	return m_current;
}

bool HeadsetImages::Fresh() const
{
	return m_presentationAvailable && m_current &&
		   FreshHeadsetImage(m_current->metadata, std::chrono::steady_clock::now());
}

std::optional<std::chrono::nanoseconds> HeadsetImages::Age(std::chrono::steady_clock::time_point now) const
{
	return m_current ? HeadsetImageAge(m_current->metadata, now) : std::nullopt;
}

void HeadsetImages::RetireUnused()
{
	std::erase_if(m_frames, [&](const auto& frame) { return frame != m_current && frame.use_count() == 1; });
}
} // namespace simxr::headset
