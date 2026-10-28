#include "control/LiveInputControl.h"
#include "Pch.h"
#include "control/ControlRequestError.h"
#include "control/ControlSchema.h"
#include "control/ControlSessions.h"
#include "control/SimulationConnection.h"
#include "control/SimulationControl.h"
#include "runtime/Session.h"

namespace simxr::control
{
std::vector<Field> EncodeLiveInput(const LiveInputSample& sample)
{

	std::vector<Field> commands{EncodeSimulationCommand(SetHeadPose{sample.head}, 1),
								EncodeSimulationCommand(SetControllerPose{0, sample.hands[0]}, 2),
								EncodeSimulationCommand(SetControllerPose{1, sample.hands[1]}, 3),
								EncodeSimulationCommand(SetControllerControls{0, sample.controls[0]}, 4),
								EncodeSimulationCommand(SetControllerControls{1, sample.controls[1]}, 5)};
	if (sample.eyeViews)
		commands.push_back(EncodeSimulationCommand(SetEyeViews{sample.eyeViews}, 6));
	if (sample.trackers)
		commands.push_back(EncodeSimulationCommand(SetTrackers{*sample.trackers}, 7));
	if (sample.body)
		commands.push_back(EncodeSimulationCommand(SetBodyControls{{}, sample.body}, 8));
	if (sample.maxAge.count() < 1 || sample.maxAge.count() > 250)
		throw std::invalid_argument("Invalid live sample age limit");
	return {Field::U64(tag::AttachmentId, sample.connection, true),
			Field::U64(tag::SampleSequence, sample.sequence, true),
			Field::U64(tag::ClockRevision, sample.clockRevision, true),
			Field::Bool(tag::Enabled, sample.controlsActive, true),
			Field::I64(tag::ObservedTime, sample.observedTime.value.count(), true),
			Field::I64(tag::PredictionTime, sample.targetTime.value.count(), true),
			Field::U32(tag::MaxSampleAgeMs, static_cast<std::uint32_t>(sample.maxAge.count()), true),
			Field::Bytes(tag::Commands, EncodeFields(commands), true)};
}
LiveInputSample DecodeLiveInput(const Message& request)
{
	LiveInputSample sample;
	sample.connection = RequireRequestU64(request, tag::AttachmentId);
	sample.sequence = RequireRequestU64(request, tag::SampleSequence);
	sample.clockRevision = RequireRequestU64(request, tag::ClockRevision);
	if (const auto enabled = request.Find(tag::Enabled))
	{
		const auto active = enabled->AsBool();
		if (!active)
			throw ControlRequestError{ErrorCode::Invalid, "Invalid live control activity"};
		sample.controlsActive = *active;
	}
	const auto observed = request.Find(tag::ObservedTime);
	const auto target = request.Find(tag::PredictionTime);
	if (!observed || !target || !observed->AsI64() || !target->AsI64())
		throw ControlRequestError{ErrorCode::Invalid, "Live sample requires native runtime timestamps"};
	sample.observedTime = {Duration{*observed->AsI64()}};
	sample.targetTime = {Duration{*target->AsI64()}};
	sample.maxAge = std::chrono::milliseconds(RequireRequestU32(request, tag::MaxSampleAgeMs));
	const auto commands = request.Find(tag::Commands);
	if (!commands || commands->value.size() > 64 * 1024)
		throw ControlRequestError{ErrorCode::Invalid, "Missing live tracking sample"};
	const auto fields = DecodeFields(commands->value);
	if (fields.error != DecodeError::None || (fields.fields.size() < 5 || fields.fields.size() > 8))
		throw ControlRequestError{ErrorCode::Invalid, "Live sample requires head and two hand poses and controls"};
	for (std::size_t index = 0; index < fields.fields.size(); ++index)
	{
		if ((index < 5 && fields.fields[index].tag != index + 1) ||
			(index >= 5 && (fields.fields[index].tag < 6 || fields.fields[index].tag > 8 ||
							fields.fields[index].tag <= fields.fields[index - 1].tag)))
			throw ControlRequestError{ErrorCode::Invalid, "Invalid live sample component order"};
		const auto command = DecodeSimulationCommand(fields.fields[index]);
		if (const auto head = std::get_if<SetHeadPose>(&command); index == 0 && head)
			sample.head = head->value;
		else if (const auto hand = std::get_if<SetControllerPose>(&command);
				 index >= 1 && index <= 2 && hand && hand->hand == index - 1)
			sample.hands[index - 1] = hand->value;
		else if (const auto controls = std::get_if<SetControllerControls>(&command);
				 index >= 3 && index <= 4 && controls && controls->hand == index - 3)
			sample.controls[index - 3] = controls->value;
		else if (const auto eyes = std::get_if<SetEyeViews>(&command);
				 fields.fields[index].tag == 6 && eyes && eyes->value)
			sample.eyeViews = eyes->value;
		else if (const auto trackers = std::get_if<SetTrackers>(&command); fields.fields[index].tag == 7 && trackers)
			sample.trackers = trackers->value;
		else if (const auto body = std::get_if<SetBodyControls>(&command);
				 fields.fields[index].tag == 8 && body && body->observation && !body->observationOrigin)
			sample.body = body->observation;
		else
			throw ControlRequestError{ErrorCode::Invalid, "Unexpected live sample command or hand"};
	}
	return sample;
}
std::optional<FrameNativeClockMapping> DecodeLiveClock(const Message& response)
{
	const auto phase = response.Find(tag::Phase);
	if (!phase || !phase->AsText())
		throw std::invalid_argument("Missing native clock phase");
	if (*phase->AsText() == "unavailable")
		return {};
	if (*phase->AsText() != "available")
		throw std::invalid_argument("Unknown native clock phase");
	const auto signedTime = [&](std::uint16_t tag)
	{
		const auto field = response.Find(tag);
		if (!field || !field->AsI64() || *field->AsI64() <= 0)
			throw std::invalid_argument("Missing or invalid native clock timestamp");
		return *field->AsI64();
	};
	FrameNativeClockMapping mapping;
	mapping.generation = {RequireRequestU64(response, tag::Generation)};
	mapping.revision = RequireRequestU64(response, tag::ClockRevision);
	mapping.domain = static_cast<NativeClockDomain>(RequireRequestU32(response, tag::ClockDomain));
	mapping.nativeNanoseconds = signedTime(tag::NativeTime);
	mapping.runtimeBefore = {Duration{signedTime(tag::RuntimeTimeBefore)}};
	mapping.runtimeAfter = {Duration{signedTime(tag::RuntimeTimeAfter)}};
	mapping.resolutionNanoseconds = signedTime(tag::ClockResolution);
	if (!mapping.generation.value || !mapping.revision || !RequireRequestU64(response, tag::AttachmentId) ||
		(mapping.domain != NativeClockDomain::MonotonicNanoseconds &&
		 mapping.domain != NativeClockDomain::PerformanceCounterNanoseconds) ||
		mapping.runtimeBefore > mapping.runtimeAfter)
		throw std::invalid_argument("Invalid native clock identity, domain or uncertainty interval");
	return mapping;
}

Message SimulationConnection::DispatchLiveInput(const Message& request, const ControlSession& binding, Message response)
{
	const auto& session = binding.session;
	const SessionGeneration generation{RequireRequestU64(request, tag::Generation)};
	const auto opcode = static_cast<OpCode>(request.opcode);
	const CommandSource source{CommandSourceKind::Device, m_connection.value};
	if (opcode == OpCode::AttachLiveInput)
	{
		if (m_liveSession)
		{
			const auto previous = m_liveSession->Snapshot();
			if (previous.closed || previous.generation != m_liveGeneration)
			{
				m_liveSession.reset();
				m_liveGeneration = {};
			}
		}
		if (m_liveSession || (m_kind && m_kind != CommandSourceKind::Device))
			throw ControlRequestError{ErrorCode::Conflict, "Use an unattached dedicated device connection"};
		if (m_attachment == std::numeric_limits<std::uint64_t>::max())
			throw ControlRequestError{ErrorCode::Capacity, "Attachment identities exhausted"};
		// Allocate the response before changing ownership of this connection.
		response.fields.push_back(Field::U64(tag::AttachmentId, m_attachment + 1, true));
		if (!session->WithCommandGeneration(generation,
											[&]
											{
												m_kind = CommandSourceKind::Device;
												Track(session, source);
												m_liveSession = session;
												m_liveGeneration = generation;
												++m_attachment;
											}))
			throw ControlRequestError{ErrorCode::StaleGeneration, "Session changed before attachment"};
		return response;
	}
	if (m_liveSession != session || m_liveGeneration != generation ||
		RequireRequestU64(request, tag::AttachmentId) != m_attachment)
		throw ControlRequestError{ErrorCode::Conflict, "Live sample attachment is absent or retired"};
	if (opcode == OpCode::PublishDisplayPrediction)
	{
		const auto target = request.Find(tag::PredictionTime);
		const auto period = request.Find(tag::Period);
		if (!target || !period || !target->AsI64() || !period->AsI64() || *target->AsI64() <= 0 ||
			*period->AsI64() < 1'000'000 || *period->AsI64() > 100'000'000)
			throw ControlRequestError{ErrorCode::Invalid, "Invalid physical display target or period"};
		const bool accepted = session->SubmitDisplayPrediction(
			generation, source, m_attachment, RequireRequestU64(request, tag::SampleSequence),
			RequireRequestU64(request, tag::ClockRevision), {Duration{*target->AsI64()}}, Duration{*period->AsI64()});
		response.fields.push_back(Field::Text(tag::Phase, accepted ? "applied" : "conflict"));
		return response;
	}
	if (opcode == OpCode::ReadLiveClock)
	{
		std::optional<FrameNativeClockMapping> mapping;
		if (!session->WithCommandGeneration(generation,
											[&]
											{
												mapping = session->Scheduler().NativeClockMapping();
											}))
			throw ControlRequestError{ErrorCode::StaleGeneration, "Session changed before clock observation"};
		response.fields.push_back(Field::U64(tag::AttachmentId, m_attachment, true));
		response.fields.push_back(Field::Text(tag::Phase, mapping ? "available" : "unavailable", true));
		if (mapping)
		{
			response.fields.push_back(Field::U32(tag::ClockDomain, static_cast<std::uint32_t>(mapping->domain), true));
			response.fields.push_back(Field::U64(tag::ClockRevision, mapping->revision, true));
			response.fields.push_back(Field::I64(tag::NativeTime, mapping->nativeNanoseconds, true));
			response.fields.push_back(Field::I64(tag::RuntimeTimeBefore, mapping->runtimeBefore.value.count(), true));
			response.fields.push_back(Field::I64(tag::RuntimeTimeAfter, mapping->runtimeAfter.value.count(), true));
			response.fields.push_back(Field::I64(tag::ClockResolution, mapping->resolutionNanoseconds, true));
		}
		return response;
	}
	if (opcode == OpCode::DetachLiveInput)
	{
		response.fields.push_back(Field::Bool(tag::Released, true));
		// Generation check also permits cleanup after End, but never the next run.
		if (!session->WithCommandGeneration(
				generation,
				[&]
				{
					session->ReleaseCommandSource(source);
				},
				false))
			throw ControlRequestError{ErrorCode::StaleGeneration, "Session changed before detach"};
		m_liveSession.reset();
		m_liveGeneration = {};
		return response;
	}
	const auto sample = DecodeLiveInput(request);
	if (sample.body && response.minor < 13)
		throw ControlRequestError{ErrorCode::UnsupportedField, "Live body input requires protocol minor 13"};
	if (sample.trackers && response.minor < 10)
		throw ControlRequestError{ErrorCode::UnsupportedField, "Live trackers require protocol minor 10"};
	const auto result = session->SubmitLiveInput(generation, source, sample);
	response.fields.push_back(Field::Text(tag::Phase, SimulationResultName(result)));
	return response;
}
} // namespace simxr::control
