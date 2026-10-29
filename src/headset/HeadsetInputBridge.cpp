#include "headset/HeadsetInputBridge.h"
#include "Pch.h"
#include "control/ControlSchema.h"
#include "control/LiveInputControl.h"
#include "control/LocalControlClient.h"
#include "control/SimulationControl.h"
#include "headset/HeadsetTracking.h"
#include "math/Math.h"

namespace simxr::headset
{
HeadsetInputBridge::HeadsetInputBridge(HeadsetInputTarget target) : m_target(std::move(target))
{
	if (m_target.endpoint.empty() || !m_target.session || !m_target.generation.value || !m_target.spaceRevision ||
		(!m_target.alignment && !NormalizedPose(m_target.worldFromPhysical)))
		throw std::invalid_argument("Native input requires endpoint, session, generation and calibration");
	if (m_target.alignment &&
		(!NormalizedPose(m_target.alignment->worldHead) || !std::isfinite(m_target.alignment->worldFloor) ||
		 (m_target.alignment->localEyeHeight &&
		  (!std::isfinite(*m_target.alignment->localEyeHeight) || *m_target.alignment->localEyeHeight <= 0))))
		throw std::invalid_argument("Invalid player alignment or eye height");
	m_worker = std::jthread([this](std::stop_token stop) { Run(stop); });
}

HeadsetInputBridge::~HeadsetInputBridge()
{
	Stop();
}

void HeadsetInputBridge::Stop()
{
	m_worker.request_stop();
	std::shared_ptr<control::LocalControlClient> client;
	{
		std::lock_guard lock(m_mutex);
		client = m_client;
	}
	m_changed.notify_all();
	if (client)
		client->Close();
	if (m_worker.joinable())
		m_worker.join();
	std::lock_guard lock(m_mutex);
	m_status.discardedSamples += m_count;
	m_count = 0;
	if (m_status.phase != HeadsetInputPhase::Failed)
		m_status.phase = HeadsetInputPhase::Stopped;
}

void HeadsetInputBridge::Fail(const char* error) noexcept
{
	std::lock_guard lock(m_mutex);
	m_status.phase = HeadsetInputPhase::Failed;
	m_status.discardedSamples += m_count;
	m_count = 0;
	try
	{
		m_status.error = error;
	}
	catch (...)
	{}
	m_changed.notify_all();
}

bool HeadsetInputBridge::Publish(const HeadsetInputSample& input, NativeClockDomain domain, std::int64_t observed,
								 std::int64_t predicted, std::optional<std::int64_t> bodyReturned)
{
	std::lock_guard lock(m_mutex);
	if (m_status.phase != HeadsetInputPhase::Ready && m_status.phase != HeadsetInputPhase::Suspended)
	{
		++m_status.rejectedSamples;
		return false;
	}
	if (m_status.phase == HeadsetInputPhase::Suspended)
	{
		// Only the newest observation can establish neutral controls on recovery.
		m_first = 0;
		m_status.discardedSamples += m_count;
		m_count = 0;
	}
	if (m_count == m_queue.size())
	{
		++m_status.rejectedSamples;
		m_status.phase = HeadsetInputPhase::Failed;
		try
		{
			m_status.error = "Native input queue overflow; reconnect to resume";
		}
		catch (...)
		{}
		m_status.discardedSamples += m_count;
		m_count = 0;
		m_changed.notify_all();
		return false;
	}
	m_queue[(m_first + m_count++) % m_queue.size()] = {
		input, domain, observed, predicted, std::chrono::steady_clock::now(), bodyReturned};
	++m_status.queuedSamples;
	m_status.queueHighWater = std::max(m_status.queueHighWater, m_count);
	m_changed.notify_one();
	return true;
}

HeadsetInputStatus HeadsetInputBridge::Status() const
{
	std::lock_guard lock(m_mutex);
	auto status = m_status;
	status.queueDepth = m_count;
	return status;
}

std::array<std::optional<HeadsetVibration>, 2> HeadsetInputBridge::ConsumeHaptics(bool enabled)
{
	std::lock_guard lock(m_mutex);
	const auto now = HeadsetHaptics::Clock::now();
	return m_haptics.Consume(now, enabled && m_status.phase == HeadsetInputPhase::Ready && now < m_inputDeadline);
}

void HeadsetInputBridge::Run(std::stop_token stop) noexcept
{
	namespace wire = control;
	std::shared_ptr<wire::LocalControlClient> client;
	try
	{
		client = std::make_shared<wire::LocalControlClient>(m_target.endpoint, std::chrono::milliseconds(250));
		{
			std::lock_guard lock(m_mutex);
			m_client = client;
		}
		const auto call = [&](wire::OpCode opcode, std::vector<wire::Field> fields = {}, bool bound = true) {
			if (stop.stop_requested())
				throw std::runtime_error("Native input stopped");
			if (bound)
			{
				fields.push_back(wire::Field::U64(wire::tag::Session, m_target.session));
				fields.push_back(wire::Field::U64(wire::tag::Generation, m_target.generation.value));
			}
			wire::ControlPacket request;
			request.message.opcode = static_cast<std::uint16_t>(opcode);
			request.message.fields = std::move(fields);
			auto response = client->Request(std::move(request)).message;
			const auto error = response.Find(wire::tag::Error);
			if (!error || error->AsU32() != static_cast<std::uint32_t>(wire::ErrorCode::Ok))
			{
				const auto detail = response.Find(wire::tag::Detail);
				throw std::runtime_error(detail && detail->AsText() ? std::string(detail->AsText()->substr(0, 256))
																	: "Native input control request failed");
			}
			if (bound && (wire::RequireRequestU64(response, wire::tag::Session) != m_target.session ||
						  wire::RequireRequestU64(response, wire::tag::Generation) != m_target.generation.value))
				throw std::runtime_error("Native input session identity changed");
			return response;
		};
		const auto phase = [](const wire::Message& response) {
			const auto field = response.Find(wire::tag::Phase);
			if (!field || !field->AsText())
				throw std::runtime_error("Native input response omitted phase");
			return std::string(*field->AsText());
		};
		call(wire::OpCode::Hello,
			 {wire::Field::Text(wire::tag::ClientName, "OpenXR headset input"),
			  wire::Field::U64(
				  wire::tag::RequiredCapabilities,
				  static_cast<std::uint64_t>(wire::Capability::LiveInput) |
					  (m_target.trackers ? static_cast<std::uint64_t>(wire::Capability::TrackerCollections) : 0) |
					  (m_target.body ? static_cast<std::uint64_t>(wire::Capability::LiveBodyInput) : 0) |
					  (m_target.displayTiming ? static_cast<std::uint64_t>(wire::Capability::ExternalDisplayTiming)
											  : 0))},
			 false);
		const auto attached = call(wire::OpCode::AttachLiveInput);
		auto attachment = wire::RequireRequestU64(attached, wire::tag::AttachmentId);
		auto clock = wire::DecodeLiveClock(
			call(wire::OpCode::ReadLiveClock, {wire::Field::U64(wire::tag::AttachmentId, attachment)}));
		if (!clock)
			throw std::runtime_error("Native input requires a running real-time clock mapping");
		std::uint64_t transaction = 0;
		const auto claim = [&](bool takeOver) {
			++transaction;
			std::vector<wire::Field> commands;
			for (const auto channel :
				 {ControlChannel::Head, ControlChannel::LeftPose, ControlChannel::RightPose,
				  ControlChannel::LeftControls, ControlChannel::RightControls, ControlChannel::Views})
				commands.push_back(wire::EncodeSimulationCommand(TakeControl{channel, takeOver},
																 static_cast<std::uint16_t>(commands.size() + 1)));
			if (m_target.trackers)
				commands.push_back(wire::EncodeSimulationCommand(TakeControl{ControlChannel::Trackers, takeOver},
																 static_cast<std::uint16_t>(commands.size() + 1)));
			if (m_target.body)
				commands.push_back(wire::EncodeSimulationCommand(TakeControl{ControlChannel::Body, takeOver},
																 static_cast<std::uint16_t>(commands.size() + 1)));
			const auto claimed = call(
				wire::OpCode::SubmitSimulation,
				{wire::Field::U32(wire::tag::SourceKind, 4), wire::Field::U64(wire::tag::TransactionId, transaction),
				 wire::Field::I64(wire::tag::ApplyTime, 0), wire::Field::U32(wire::tag::LatePolicy, 1),
				 wire::Field::Bytes(wire::tag::Commands, wire::EncodeFields(commands))});
			if (phase(claimed) != "queued" && phase(claimed) != "rescheduled")
				return false;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			for (;;)
			{
				const auto status = phase(
					call(wire::OpCode::SimulationStatus, {wire::Field::U64(wire::tag::TransactionId, transaction)}));
				if (status == "applied")
					break;
				if ((status != "queued" && status != "rescheduled") || std::chrono::steady_clock::now() >= deadline)
					return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return true;
		};
		if (!claim(m_target.takeOver))
			throw std::runtime_error(
				"Native input ownership unavailable; resume the game or release conflicting channels");
		bool suspended = false;
		bool requireNeutral = m_target.requireNeutral;
		const auto pollHaptics = [&] {
			const auto requested = HeadsetHaptics::Clock::now();
			const auto observation = wire::DecodeHaptics(call(wire::OpCode::ReadHaptics));
			const auto received = HeadsetHaptics::Clock::now();
			std::lock_guard lock(m_mutex);
			if (!m_haptics.Observe(observation, requested, received, !suspended && !requireNeutral))
				throw std::runtime_error("Physical haptic observation lost continuity; reconnect to resume");
		};
		pollHaptics();
		auto nextHaptics = HeadsetHaptics::Clock::now() + std::chrono::milliseconds(5);
		{
			std::lock_guard lock(m_mutex);
			m_status.phase = HeadsetInputPhase::Ready;
		}
		bool resumeAllowed = true;
		const auto clockChanged = [&] {
			const auto current = wire::DecodeLiveClock(
				call(wire::OpCode::ReadLiveClock, {wire::Field::U64(wire::tag::AttachmentId, attachment)}));
			if (current && current->revision == clock->revision && current->domain == clock->domain)
				return false;
			call(wire::OpCode::DetachLiveInput, {wire::Field::U64(wire::tag::AttachmentId, attachment)});
			attachment = wire::RequireRequestU64(call(wire::OpCode::AttachLiveInput), wire::tag::AttachmentId);
			suspended = true;
			clock.reset();
			std::lock_guard lock(m_mutex);
			m_status.phase = HeadsetInputPhase::Suspended;
			m_status.error = "Waiting for simulator clock and neutral controls";
			m_status.discardedSamples += m_count;
			m_count = 0;
			m_inputDeadline = {};
			return true;
		};
		std::uint64_t sequence = 0;
		auto calibration = m_target.worldFromPhysical;
		bool aligned = !m_target.alignment;
		auto alignmentDeadline = HeadsetHaptics::Clock::now() + std::chrono::seconds(2);
		while (!stop.stop_requested())
		{
			if (!suspended && !aligned && !requireNeutral && HeadsetHaptics::Clock::now() >= alignmentDeadline)
				throw std::runtime_error("Head tracking unavailable for initial alignment; reconnect to retry");
			std::optional<Observation> observation;
			{
				std::unique_lock lock(m_mutex);
				m_changed.wait_until(lock, nextHaptics, [&] {
					return stop.stop_requested() || m_count || m_status.phase == HeadsetInputPhase::Failed;
				});
				if (stop.stop_requested() || m_status.phase == HeadsetInputPhase::Failed)
					break;
				if (m_count)
				{
					observation = m_queue[m_first];
					m_first = (m_first + 1) % m_queue.size();
					--m_count;
					++m_status.dequeuedSamples;
					m_status.maximumQueueDelay = std::max(
						m_status.maximumQueueDelay,
						std::chrono::duration_cast<Duration>(std::chrono::steady_clock::now() - observation->received));
				}
			}
			if (suspended)
			{
				if (HeadsetHaptics::Clock::now() >= nextHaptics)
				{
					clock = wire::DecodeLiveClock(
						call(wire::OpCode::ReadLiveClock, {wire::Field::U64(wire::tag::AttachmentId, attachment)}));
					pollHaptics();
					nextHaptics = HeadsetHaptics::Clock::now() + std::chrono::milliseconds(5);
				}
				if (!resumeAllowed || !clock || !observation ||
					HeadsetHaptics::Clock::now() - observation->received > std::chrono::milliseconds(50) ||
					!observation->input.focused || !observation->input.controlsReady ||
					(observation->input.head.locationFlags & ValidPoseFlags) != ValidPoseFlags ||
					!std::all_of(observation->input.hands.begin(), observation->input.hands.end(),
								 NeutralHeadsetControls))
					continue;
				if (!claim(false))
				{
					call(wire::OpCode::DetachLiveInput, {wire::Field::U64(wire::tag::AttachmentId, attachment)});
					attachment = wire::RequireRequestU64(call(wire::OpCode::AttachLiveInput), wire::tag::AttachmentId);
					resumeAllowed = false;
					std::lock_guard lock(m_mutex);
					m_status.error = "Input ownership unavailable; reconnect to retry";
					continue;
				}
				pollHaptics();
				suspended = false;
				requireNeutral = true;
				alignmentDeadline = HeadsetHaptics::Clock::now() + std::chrono::seconds(2);
				// Drop observations collected while the claim was pending. Require a fresh
				// neutral sample again before enabling forwarding.
				std::lock_guard lock(m_mutex);
				m_status.discardedSamples += m_count;
				m_count = 0;
				continue;
			}
			if (observation && requireNeutral &&
				(!observation->input.focused || !observation->input.controlsReady ||
				 (observation->input.head.locationFlags & ValidPoseFlags) != ValidPoseFlags ||
				 !std::all_of(observation->input.hands.begin(), observation->input.hands.end(),
							  NeutralHeadsetControls)))
				observation.reset();
			if (observation)
			{
				if (std::chrono::steady_clock::now() - observation->received > std::chrono::milliseconds(50))
					throw std::runtime_error("Native input backlog expired; reconnect to resume");
				if (!aligned)
				{
					if ((observation->input.head.locationFlags & ValidPoseFlags) != ValidPoseFlags)
					{
						if (HeadsetHaptics::Clock::now() >= alignmentDeadline)
							throw std::runtime_error(
								"Head tracking unavailable for initial alignment; reconnect to retry");
						continue;
					}
					calibration = AlignHeadset(observation->input, *m_target.alignment);
					aligned = true;
				}
				if (m_target.trackers && !observation->input.trackers)
					throw std::runtime_error("Physical tracker provider omitted its inventory");
				if (!m_target.trackers)
					observation->input.trackers.reset();
				if (m_target.body && !observation->input.body)
					throw std::runtime_error("Physical body provider omitted its observation");
				if (!m_target.body)
				{
					observation->input.body.reset();
					observation->bodyReturned.reset();
				}
				const auto sample =
					MapHeadsetTracking(observation->input, *clock, observation->domain, observation->observed,
									   observation->predicted, calibration, attachment, ++sequence,
									   std::chrono::milliseconds(1), m_target.spaceRevision, observation->bodyReturned);
				const auto result = phase(call(wire::OpCode::SubmitLiveInput, wire::EncodeLiveInput(sample)));
				if (result != "queued" && result != "applied")
				{
					if (clockChanged())
						continue;
					throw std::runtime_error("Native input ownership was lost; reconnect to resume");
				}
				bool predicted = false;
				if (m_target.displayTiming)
					predicted =
						phase(call(wire::OpCode::PublishDisplayPrediction,
								   {wire::Field::U64(wire::tag::AttachmentId, attachment),
									wire::Field::U64(wire::tag::SampleSequence, sequence),
									wire::Field::U64(wire::tag::ClockRevision, sample.clockRevision),
									wire::Field::I64(wire::tag::PredictionTime, sample.targetTime.value.count()),
									wire::Field::I64(wire::tag::Period, observation->input.displayPeriod)})) ==
						"applied";
				requireNeutral = false;
				std::lock_guard lock(m_mutex);
				m_status.predictions += predicted;
				m_status.phase = HeadsetInputPhase::Ready;
				m_status.error.clear();
				++m_status.sent;
				m_status.maximumForwardAge = std::max(
					m_status.maximumForwardAge,
					std::chrono::duration_cast<Duration>(std::chrono::steady_clock::now() - observation->received));
				m_status.worldFromInitial = calibration;
				m_inputDeadline = observation->received + std::chrono::milliseconds(50);
			}
			if (HeadsetHaptics::Clock::now() >= nextHaptics)
			{
				if (clockChanged())
					continue;
				pollHaptics();
				nextHaptics = HeadsetHaptics::Clock::now() + std::chrono::milliseconds(5);
			}
		}
	}
	catch (const std::exception& error)
	{
		if (!stop.stop_requested())
			Fail(error.what());
	}
	catch (...)
	{
		if (!stop.stop_requested())
			Fail("Native input worker failed");
	}
	if (client)
		client->Close();
}
} // namespace simxr::headset
