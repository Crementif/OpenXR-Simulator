#include "control/SimulationControl.h"

#include <span>
#include <string_view>
#include <utility>

namespace simxr::control
{
namespace
{
InspectionValue Vector(const XrVector3f& vector)
{
	return InspectionValue::Array(
		{InspectionValue::Float(vector.x), InspectionValue::Float(vector.y), InspectionValue::Float(vector.z)});
}
InspectionValue Orientation(const XrQuaternionf& orientation)
{
	return InspectionValue::Array({InspectionValue::Float(orientation.x), InspectionValue::Float(orientation.y),
								   InspectionValue::Float(orientation.z), InspectionValue::Float(orientation.w)});
}
InspectionValue PoseValue(const PoseSample& sample)
{
	return InspectionValue::Object({{"position", Vector(sample.pose.position)},
									{"orientation", Orientation(sample.pose.orientation)},
									{"flags", InspectionValue::Unsigned(sample.flags)}});
}
InspectionValue PoseValue(const XrPosef& pose)
{
	return InspectionValue::Object(
		{{"position", Vector(pose.position)}, {"orientation", Orientation(pose.orientation)}});
}
InspectionValue ReferenceSpaces(const ReferenceSpaceState& spaces)
{
	return InspectionValue::Object(
		{{"local", PoseValue(spaces.local)},
		 {"stage", PoseValue(spaces.stage)},
		 {"stage_bounds", spaces.stageBounds
							  ? InspectionValue::Array({InspectionValue::Float(spaces.stageBounds->width),
														InspectionValue::Float(spaces.stageBounds->height)})
							  : InspectionValue::Null()}});
}
InspectionValue ControlValue(const ControllerControls& controls)
{
	return InspectionValue::Object({{"trigger", InspectionValue::Float(controls.trigger)},
									{"grip", InspectionValue::Float(controls.grip)},
									{"grip_force", InspectionValue::Float(controls.gripForce)},
									{"stick", InspectionValue::Array({InspectionValue::Float(controls.stick.x),
																	  InspectionValue::Float(controls.stick.y)})},
									{"trackpad", InspectionValue::Array({InspectionValue::Float(controls.trackpad.x),
																		 InspectionValue::Float(controls.trackpad.y)})},
									{"trackpad_force", InspectionValue::Float(controls.trackpadForce)},
									{"primary", InspectionValue::Boolean(controls.primary)},
									{"secondary", InspectionValue::Boolean(controls.secondary)},
									{"menu", InspectionValue::Boolean(controls.menu)},
									{"back", InspectionValue::Boolean(controls.back)},
									{"thumb_click", InspectionValue::Boolean(controls.thumbClick)},
									{"trackpad_click", InspectionValue::Boolean(controls.trackpadClick)},
									{"primary_touch", InspectionValue::Boolean(controls.primaryTouch)},
									{"secondary_touch", InspectionValue::Boolean(controls.secondaryTouch)},
									{"trigger_touch", InspectionValue::Boolean(controls.triggerTouch)},
									{"thumb_touch", InspectionValue::Boolean(controls.thumbTouch)},
									{"trackpad_touch", InspectionValue::Boolean(controls.trackpadTouch)},
									{"thumbrest_touch", InspectionValue::Boolean(controls.thumbrestTouch)},
									{"connected", InspectionValue::Boolean(controls.connected)}});
}
InspectionValue SourceValue(CommandSource source)
{
	return InspectionValue::Object({{"kind", InspectionValue::Unsigned(static_cast<std::uint32_t>(source.kind))},
									{"id", InspectionValue::Unsigned(source.id)}});
}
InspectionValue BodyValue(const BodyControls& body)
{
	std::vector<InspectionValue> controls, joints;
	for (const auto& control : body.controls)
		controls.push_back(
			InspectionValue::Object({{"tracked", InspectionValue::Boolean(control.tracked)},
									 {"offset", control.offset ? Vector(*control.offset) : InspectionValue::Null()}}));
	for (std::size_t index = 0; index < body.joints.size(); ++index)
		if (const auto& joint = body.joints[index])
			joints.push_back(
				InspectionValue::Object({{"joint", InspectionValue::Unsigned(index)},
										 {"tracked", InspectionValue::Boolean(joint->tracked)},
										 {"pose", joint->pose ? PoseValue(*joint->pose) : InspectionValue::Null()}}));
	return InspectionValue::Object({{"active", InspectionValue::Boolean(body.active)},
									{"confidence", InspectionValue::Float(body.confidence)},
									{"preset", InspectionValue::Unsigned(static_cast<std::uint32_t>(body.preset))},
									{"controls", InspectionValue::Array(std::move(controls))},
									{"joints", InspectionValue::Array(std::move(joints))}});
}
InspectionValue Configuration(const CommandConfiguration& configuration)
{
	std::vector<InspectionValue> controllers, trackers, outerFov;
	for (const auto& fov : configuration.views.outerFov)
		for (const auto angle : {fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown})
			outerFov.push_back(InspectionValue::Float(angle));
	for (std::size_t index = 0; index < configuration.tracking.controllers.size(); ++index)
		controllers.push_back(InspectionValue::Object(
			{{"pose", PoseValue(configuration.tracking.controllers[index].pose)},
			 {"connected", InspectionValue::Boolean(configuration.tracking.controllers[index].connected)},
			 {"controls", ControlValue(configuration.input.controllers[index])},
			 {"profile", InspectionValue::Text(configuration.input.preferredProfiles[index])}}));
	for (const auto& tracker : configuration.tracking.trackers)
		trackers.push_back(InspectionValue::Object(
			{{"id", InspectionValue::Unsigned(tracker.identity.value)},
			 {"role", InspectionValue::Unsigned(static_cast<std::uint32_t>(tracker.role))},
			 {"connected", InspectionValue::Boolean(tracker.connected)},
			 {"tracked", InspectionValue::Boolean(tracker.tracked)},
			 {"anchor", tracker.anchor ? InspectionValue::Unsigned(static_cast<std::uint32_t>(*tracker.anchor))
									   : InspectionValue::Null()},
			 {"pose_override", tracker.poseOverride ? PoseValue(*tracker.poseOverride) : InspectionValue::Null()}}));
	return InspectionValue::Object(
		{{"head", PoseValue(configuration.tracking.head)},
		 {"controllers", InspectionValue::Array(std::move(controllers))},
		 {"gaze",
		  InspectionValue::Object({{"yaw", InspectionValue::Float(configuration.tracking.gaze.yaw)},
								   {"pitch", InspectionValue::Float(configuration.tracking.gaze.pitch)},
								   {"available", InspectionValue::Boolean(configuration.tracking.gaze.available)},
								   {"tracked", InspectionValue::Boolean(configuration.tracking.gaze.tracked)}})},
		 {"body", BodyValue(configuration.tracking.body)},
		 {"trackers", InspectionValue::Array(std::move(trackers))},
		 {"focused", InspectionValue::Boolean(configuration.tracking.focused)},
		 {"reference_spaces", ReferenceSpaces(configuration.referenceSpaces)},
		 {"views",
		  InspectionValue::Object({{"ipd", InspectionValue::Float(configuration.views.ipd)},
								   {"outer_fov", InspectionValue::Array(std::move(outerFov))},
								   {"outer_width", InspectionValue::Unsigned(configuration.views.outerWidth)},
								   {"outer_height", InspectionValue::Unsigned(configuration.views.outerHeight)},
								   {"inset_width", InspectionValue::Unsigned(configuration.views.insetWidth)},
								   {"inset_height", InspectionValue::Unsigned(configuration.views.insetHeight)},
								   {"inset_half_angle", InspectionValue::Float(configuration.views.insetHalfAngle)}})},
		 {"proportions",
		  InspectionValue::Object({{"height", InspectionValue::Float(configuration.proportions.height)},
								   {"shoulder_width", InspectionValue::Float(configuration.proportions.shoulderWidth)},
								   {"hip_width", InspectionValue::Float(configuration.proportions.hipWidth)}})}});
}
InspectionValue EncodedCommand(const SimulationCommand& command)
{
	// The editable recording retains the exact canonical command TLV as hex.
	// SDK and rigs decode this with the same public codec; no process-local handles.
	const auto field = EncodeSimulationCommand(command);
	constexpr std::string_view digits = "0123456789abcdef";
	std::string hex;
	hex.reserve(field.value.size() * 2);
	for (const auto byte : field.value)
	{
		const auto value = std::to_integer<unsigned>(byte);
		hex.push_back(digits[value >> 4]);
		hex.push_back(digits[value & 15]);
	}
	return InspectionValue::Object(
		{{"encoding", InspectionValue::Text("simxr-command-v1-hex")}, {"data", InspectionValue::Text(hex)}});
}
} // namespace

const char* SimulationResultName(CommandResult result)
{
	switch (result)
	{
	case CommandResult::Queued:
		return "queued";
	case CommandResult::Applied:
		return "applied";
	case CommandResult::Late:
		return "late";
	case CommandResult::Rescheduled:
		return "rescheduled";
	case CommandResult::Conflict:
		return "conflict";
	case CommandResult::Cancelled:
		return "cancelled";
	case CommandResult::Invalid:
		return "invalid";
	case CommandResult::Capacity:
		return "capacity";
	case CommandResult::Closed:
		return "closed";
	case CommandResult::Released:
		return "released";
	case CommandResult::NotFound:
		return "not_found";
	}
	return "invalid";
}
InspectionValue SimulationOutcome(const CommandOutcome& outcome, bool includeCommands)
{
	std::vector<InspectionValue> commands;
	if (includeCommands)
		for (const auto& command : outcome.transaction.commands)
			commands.push_back(EncodedCommand(command));
	return InspectionValue::Object(
		{{"transaction_id", InspectionValue::Unsigned(outcome.transaction.id)},
		 {"source", SourceValue(outcome.transaction.source)},
		 {"phase", InspectionValue::Text(SimulationResultName(outcome.result))},
		 {"applied_frame", InspectionValue::Unsigned(outcome.appliedFrame.value)},
		 {"after_frame", InspectionValue::Boolean(outcome.afterFrame)},
		 {"applied_time", InspectionValue::Signed(outcome.appliedTime.value.count())},
		 {"recording_overrun", InspectionValue::Boolean(outcome.recordingOverrun)},
		 {"target_frame", outcome.transaction.target.frame
							  ? InspectionValue::Unsigned(outcome.transaction.target.frame->value)
							  : InspectionValue::Null()},
		 {"target_time", outcome.transaction.target.time
							 ? InspectionValue::Signed(outcome.transaction.target.time->value.count())
							 : InspectionValue::Null()},
		 {"effective_frame", outcome.effectiveTarget && outcome.effectiveTarget->frame
								 ? InspectionValue::Unsigned(outcome.effectiveTarget->frame->value)
								 : InspectionValue::Null()},
		 {"effective_time", outcome.effectiveTarget && outcome.effectiveTarget->time
								? InspectionValue::Signed(outcome.effectiveTarget->time->value.count())
								: InspectionValue::Null()},
		 {"late_policy", InspectionValue::Unsigned(static_cast<std::uint32_t>(outcome.transaction.latePolicy))},
		 {"commands", InspectionValue::Array(std::move(commands))}});
}
InspectionValue SimulationState(const CommandSchedulerSnapshot& state, bool includeCommands)
{
	std::vector<InspectionValue> outcomes;
	auto recent = std::span{state.outcomes};
	if (!includeCommands && recent.size() > 32)
		recent = recent.last(32);
	for (const auto& outcome : recent)
		outcomes.push_back(SimulationOutcome(outcome, includeCommands));
	std::vector<std::pair<std::string, InspectionValue>> fields{
		{"seed", InspectionValue::Unsigned(state.seed)},
		{"closed", InspectionValue::Boolean(state.closed)},
		{"pending", InspectionValue::Unsigned(state.pending)},
		{"transactions", InspectionValue::Array(std::move(outcomes))}};
	if (state.applied)
	{
		const auto& applied = *state.applied;
		std::vector<InspectionValue> owners, scenarios;
		for (const auto& owner : applied.owners)
			owners.push_back(owner ? SourceValue(*owner) : InspectionValue::Null());
		for (std::size_t index = 0; index < applied.scenarios.size(); ++index)
			if (applied.scenarios[index])
				scenarios.push_back(InspectionValue::Unsigned(index));

		fields.emplace_back("revision", InspectionValue::Unsigned(applied.revision));
		fields.emplace_back("frame_id", InspectionValue::Unsigned(applied.frame.value));
		fields.emplace_back("time", InspectionValue::Signed(applied.time.value.count()));
		fields.emplace_back("owners", InspectionValue::Array(std::move(owners)));
		fields.emplace_back("scenarios", InspectionValue::Array(std::move(scenarios)));
		fields.emplace_back("configuration_available", InspectionValue::Boolean(applied.configurationAvailable));
		fields.emplace_back("configuration", applied.configurationAvailable ? Configuration(applied.configuration)
																			: InspectionValue::Null());
	}
	return InspectionValue::Object(std::move(fields));
}
InspectionValue SimulationRecording(const CommandRecording& recording)
{
	std::vector<InspectionValue> initial;
	const auto& configuration = recording.origin.configuration;
	const auto add = [&](SimulationCommand command)
	{
		initial.push_back(EncodedCommand(command));
	};
	add(SetHeadPose{configuration.tracking.head});
	for (std::uint32_t hand = 0; hand < 2; ++hand)
	{
		add(SetControllerPose{hand, configuration.tracking.controllers[hand]});
		add(SetControllerControls{hand, configuration.input.controllers[hand]});
		add(SetControllerProfile{hand, configuration.input.preferredProfiles[hand]});
	}
	add(SetBodyControls{configuration.tracking.body});
	add(SetGaze{configuration.tracking.gaze});
	for (std::uint32_t index = 0; index < configuration.tracking.trackers.size(); ++index)
		add(SetTracker{index, configuration.tracking.trackers[index]});
	add(SetViewSettings{configuration.views});
	add(SetBodyProportions{configuration.proportions});
	add(SetReferenceSpaces{configuration.referenceSpaces});
	std::vector<InspectionValue> scenarios;
	for (std::size_t index = 0; index < recording.origin.scenarios.size(); ++index)
		if (recording.origin.scenarios[index])
			scenarios.push_back(InspectionValue::Unsigned(index));
	std::vector<InspectionValue> outcomes;
	for (const auto& outcome : recording.transactions)
		outcomes.push_back(SimulationOutcome(outcome));
	return InspectionValue::Object({{"version", InspectionValue::Unsigned(recording.version)},
									{"active", InspectionValue::Boolean(recording.active)},
									{"overflow", InspectionValue::Boolean(recording.overflow)},
									{"seed", InspectionValue::Unsigned(recording.origin.seed)},
									{"origin_frame", InspectionValue::Unsigned(recording.origin.frame.value)},
									{"origin_time", InspectionValue::Signed(recording.origin.time.value.count())},
									{"initial", Configuration(recording.origin.configuration)},
									{"initial_commands", InspectionValue::Array(std::move(initial))},
									{"initial_scenarios", InspectionValue::Array(std::move(scenarios))},
									{"total", InspectionValue::Unsigned(recording.total)},
									{"transactions", InspectionValue::Array(std::move(outcomes))}});
}
} // namespace simxr::control
