#include "app/services/FrameHistoryFeed.h"
#include "Pch.h"
#include "control/SimulationControl.h"

namespace simxr::app
{
static std::size_t InputBudget(const HistoryStore& history)
{
	constexpr std::size_t budget = 64 * 1024 * 1024;
	if (history.RetainedBytes() >= budget)
		throw std::length_error("Timeline records exceed the CPU history budget");
	return budget - history.RetainedBytes();
}
FrameHistoryFeed::FrameHistoryFeed() : m_input(32768, InputBudget(m_history)) {}
std::size_t FrameHistoryFeed::RetainedBytes() const noexcept
{
	return m_history.RetainedBytes() + m_input.RetainedBytes();
}
static InputSample ReadInput(const control::Message& applied)
{
	using namespace panel;
	namespace tag = control::tag;
	InputSample sample;
	TrackerDevices trackers(4);
	bool collection = false;
	std::uint32_t trackerSlots = 0, devices = 0;
	// Fields 1-11 are input commands; the named fields follow at higher tags.
	for (const auto& field : applied.fields)
	{
		if (field.tag > 11 && field.tag != tag::TrackerCollection)
			continue;
		const auto command = control::DecodeSimulationCommand(field);
		if (field.tag == tag::TrackerCollection)
		{
			const auto* value = std::get_if<SetTrackers>(&command);
			if (!value || collection || trackerSlots)
				throw std::runtime_error("Invalid or mixed tracker inventory");
			trackers = value->value;
			collection = true;
			continue;
		}
		std::uint32_t bit = 0;
		if (const auto* head = std::get_if<SetHeadPose>(&command))
		{
			sample.head = head->value;
			bit = 1;
		}
		else if (const auto* pose = std::get_if<SetControllerPose>(&command); pose && pose->hand < 2)
		{
			sample.poses[pose->hand] = pose->value;
			bit = 2u << pose->hand;
		}
		else if (const auto* controls = std::get_if<SetControllerControls>(&command); controls && controls->hand < 2)
		{
			sample.controls[controls->hand] = controls->value;
			bit = 8u << controls->hand;
		}
		else if (const auto* body = std::get_if<SetBodyControls>(&command))
		{
			if (body->observation)
				sample.bodyObservation = std::make_shared<const BodyObservation>(*body->observation);
			bit = 32;
			auto shared = body->value;
			sample.bodyRoot = std::exchange(shared.rootPose, std::nullopt);
			sample.body = std::make_shared<const BodyControls>(std::move(shared));
		}
		else if (const auto* gaze = std::get_if<SetGaze>(&command))
		{
			sample.gaze = gaze->value;
			bit = 64;
		}
		else if (const auto* tracker = std::get_if<SetTracker>(&command); tracker && !collection && tracker->index < 4)
		{
			bit = 128u << tracker->index;
			trackers[tracker->index] = tracker->value;
			trackerSlots |= 1u << tracker->index;
		}
		else
			throw std::runtime_error("Frame history has an unexpected input command");
		if (devices & bit)
			throw std::runtime_error("Frame history repeats a device input");
		devices |= bit;
	}
	sample.complete = collection ? devices == 127 : devices == (1u << 11) - 1;
	if (collection || trackerSlots == 15)
		sample.trackers = std::make_shared<const TrackerDevices>(trackers);
	if (const auto* context = applied.Find(tag::InputContext))
		sample.context = std::make_shared<const std::vector<std::byte>>(context->value);
	sample.seed = U64(applied, tag::Seed);
	const auto scenarios = U32(applied, tag::Scenario);
	for (std::size_t index = 0; index < sample.scenarios.size(); ++index)
		sample.scenarios[index] = scenarios & (1u << index);
	sample.driven = U32(applied, tag::Ownership);
	sample.actions = U32(applied, tag::ActiveActions);
	return sample;
}
void FrameHistoryFeed::SetScope(PanelScope scope)
{
	if (scope == m_scope)
		return;
	m_scope = scope;
	m_unreadable = m_skipped = false;
	m_after = 0;
	if (const auto previous = m_history.Latest({scope.session, scope.generation, 0}, scope.epoch))
		m_after = m_history.At(*previous).identity.frame;
	m_newSegment = true;
	m_offset = m_history.Size() ? m_history.At(m_history.Size() - 1).seconds + .1 : 0;
}
std::string FrameHistoryFeed::Accept(const control::Message& message)
{
	using namespace panel;
	namespace tag = control::tag;
	if (!m_scope.session || U64(message, tag::Session) != m_scope.session ||
		U64(message, tag::Generation) != m_scope.generation)
		return {};
	if (m_unreadable)
		return {};
	control::Message frames;
	try
	{
		if (!message.Find(tag::Frames))
			throw std::runtime_error("Frame history has no frame list");
		frames = Nested(message.Find(tag::Frames));
	}
	catch (const std::exception& error)
	{
		m_unreadable = m_skipped = true;
		return std::string(error.what()) + ". History stopped. Reconnect to resume.";
	}
	try
	{
		Append(message, frames);
		return {};
	}
	catch (const std::exception& error)
	{
		// Skip identifiable records. Never invent a cursor for undecodable data.
		for (const auto& entry : frames.fields)
		{
			auto decoded = control::DecodeFields(entry.value);
			control::Message frame;
			frame.fields = std::move(decoded.fields);
			const auto id = U64(frame, tag::FrameId);
			if (decoded.error != control::DecodeError::None || !id)
				m_unreadable = true;
			else
				m_after = std::max(m_after, id);
		}
		m_skipped = true;
		return std::string(error.what()) + (m_unreadable ? ". History stopped. Reconnect to resume." : "");
	}
}
void FrameHistoryFeed::Append(const control::Message& message, const control::Message& frames)
{
	using namespace panel;
	namespace tag = control::tag;
	if (frames.fields.size() > 128)
		throw std::runtime_error("Frame history response exceeds its page limit");
	for (const auto& entry : frames.fields)
	{
		const auto frame = Nested(&entry);
		const auto id = U64(frame, tag::FrameId);
		if (!id)
			throw std::runtime_error("Frame history has no frame identity");
		if (id <= m_after)
			continue;
		if (U64(frame, tag::Generation) != m_scope.generation)
			throw std::runtime_error("Frame history contains another run");
		const auto prediction = Nested(frame.Find(tag::Prediction));
		const auto* time = prediction.Find(tag::Time);
		const auto* period = prediction.Find(tag::Period);
		if (!time || !time->AsI64() || !period || !period->AsI64() || *period->AsI64() <= 0)
			throw std::runtime_error("Frame history has invalid runtime timing");
		const auto timestamp = *time->AsI64();
		std::uint32_t flags = 0;
		auto originTime = m_origin;
		auto offset = m_offset;
		if (m_newSegment)
		{
			const auto origin = message.Find(tag::Time);
			originTime = origin && origin->AsI64() ? *origin->AsI64() : timestamp;
			offset -= double((static_cast<long double>(timestamp) - originTime) / 1000000000.L);
			flags |= HistoryRunStart;
		}
		if ((m_after && id - m_after > 1) || (!m_newSegment && timestamp < m_previous) || m_skipped)
			flags |= HistoryGap;
		const auto submission = Nested(frame.Find(tag::Submission));
		if (Text(submission, tag::OutputDisposition) == "dropped" || Text(frame, tag::Stage) == "discarded")
			flags |= HistoryDropped;
		std::uint64_t input = 0;
		std::array<std::uint8_t, 2> hands{};
		if (const auto* applied = frame.Find(tag::AppliedInput))
		{
			auto sample = ReadInput(Nested(applied));
			sample.identity = {m_scope.session, m_scope.generation, id};
			sample.epoch = m_scope.epoch;
			sample.runtimeTime = RuntimeTime{Duration{timestamp}};
			for (std::size_t hand = 0; hand < 2; ++hand)
				hands[hand] = static_cast<std::uint8_t>(std::lround(Intensity(sample.controls[hand]) * 255));
			constexpr auto Pointing = 1u << static_cast<int>(ControlChannel::Head) |
									  1u << static_cast<int>(ControlChannel::LeftPose) |
									  1u << static_cast<int>(ControlChannel::RightPose) |
									  1u << static_cast<int>(ControlChannel::LeftControls) |
									  1u << static_cast<int>(ControlChannel::RightControls);
			if (sample.driven & Pointing)
				flags |= HistoryDriven;
			if (sample.actions)
				flags |= HistoryAction;
			input = m_input.Append(std::move(sample));
		}
		const auto periodMilliseconds = static_cast<long double>(*period->AsI64()) / 1000000.L;
		const auto elapsed = double((static_cast<long double>(timestamp) - originTime) / 1000000000.L);
		const auto previousSeconds = m_history.Size() ? m_history.At(m_history.Size() - 1).seconds : m_offset;
		const auto interval =
			!m_newSegment ? (static_cast<long double>(timestamp) - m_previous) / 1000000.L : periodMilliseconds;
		// A display time more than half a period late means the app missed its frame;
		// a gap of a second or more is a pause or stall, not a missed frame.
		if (interval > periodMilliseconds * 1.5L && interval < 1000)
			flags |= HistoryLate;
		const auto milliseconds = [&](std::uint16_t tag)
		{
			const auto* value = frame.Find(tag);
			const auto ns = value ? value->AsI64() : std::nullopt;
			return ns && *ns >= 0 ? float(static_cast<long double>(*ns) / 1000000.L) : -1.f;
		};
		m_history.Append({{m_scope.session, m_scope.generation, id},
						  m_scope.epoch,
						  0,
						  std::max(previousSeconds, offset + elapsed),
						  float(std::max(0.L, interval)),
						  flags,
						  elapsed,
						  input,
						  hands,
						  float(periodMilliseconds),
						  milliseconds(tag::CpuFrameTime),
						  milliseconds(tag::CompositorGpuTime)});
		m_origin = originTime;
		m_offset = offset;
		m_skipped = false;
		m_previous = timestamp;
		m_after = id;
		m_newSegment = false;
	}
}
} // namespace simxr::app
