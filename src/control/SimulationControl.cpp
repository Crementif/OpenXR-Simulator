#include "control/SimulationControl.h"
#include "Pch.h"

namespace simxr::control
{
static InspectionValue Vector(const XrVector3f& vector)
{
	return InspectionValue::Array(
		{InspectionValue::Float(vector.x), InspectionValue::Float(vector.y), InspectionValue::Float(vector.z)});
}

static InspectionValue Orientation(const XrQuaternionf& orientation)
{
	return InspectionValue::Array({InspectionValue::Float(orientation.x), InspectionValue::Float(orientation.y),
								   InspectionValue::Float(orientation.z), InspectionValue::Float(orientation.w)});
}

static InspectionValue PoseValue(const PoseSample& sample)
{
	return InspectionValue::Object({{"position", Vector(sample.pose.position)},
									{"orientation", Orientation(sample.pose.orientation)},
									{"flags", InspectionValue::Unsigned(sample.flags)}});
}

static InspectionValue PoseValue(const XrPosef& pose)
{
	return InspectionValue::Object(
		{{"position", Vector(pose.position)}, {"orientation", Orientation(pose.orientation)}});
}

static InspectionValue EyeViews(const std::optional<StereoEyeViews>& views)
{
	if (!views)
		return InspectionValue::Null();
	std::vector<InspectionValue> eyes;
	for (std::size_t eye = 0; eye < views->poses.size(); ++eye)
	{
		const auto& fov = views->fovs[eye];
		eyes.push_back(InspectionValue::Object(
			{{"pose", PoseValue(views->poses[eye])},
			 {"fov",
			  InspectionValue::Array({InspectionValue::Float(fov.angleLeft), InspectionValue::Float(fov.angleRight),
									  InspectionValue::Float(fov.angleUp), InspectionValue::Float(fov.angleDown)})}}));
	}
	return InspectionValue::Object({{"space_revision", InspectionValue::Unsigned(views->spaceRevision)},
									{"eyes", InspectionValue::Array(std::move(eyes))}});
}

static InspectionValue ReferenceSpaces(const ReferenceSpaceState& spaces)
{
	return InspectionValue::Object(
		{{"local", PoseValue(spaces.local)},
		 {"stage", PoseValue(spaces.stage)},
		 {"stage_bounds", spaces.stageBounds
							  ? InspectionValue::Array({InspectionValue::Float(spaces.stageBounds->width),
														InspectionValue::Float(spaces.stageBounds->height)})
							  : InspectionValue::Null()}});
}

static InspectionValue ControlValue(const ControllerControls& controls)
{
	return InspectionValue::Object(
		{{"trigger", InspectionValue::Float(controls.trigger)},
		 {"grip", InspectionValue::Float(controls.grip)},
		 {"grip_force", InspectionValue::Float(controls.gripForce)},
		 {"stick",
		  InspectionValue::Array({InspectionValue::Float(controls.stick.x), InspectionValue::Float(controls.stick.y)})},
		 {"trackpad", InspectionValue::Array(
						  {InspectionValue::Float(controls.trackpad.x), InspectionValue::Float(controls.trackpad.y)})},
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
		 {"physical_profile", InspectionValue::Text(controls.physicalProfile)},
		 {"trigger_click",
		  controls.triggerClick ? InspectionValue::Boolean(*controls.triggerClick) : InspectionValue::Null()},
		 {"thumb_touch", InspectionValue::Boolean(controls.thumbTouch)},
		 {"trackpad_touch", InspectionValue::Boolean(controls.trackpadTouch)},
		 {"thumbrest_touch", InspectionValue::Boolean(controls.thumbrestTouch)},
		 {"connected", InspectionValue::Boolean(controls.connected)}});
}

static InspectionValue SourceValue(CommandSource source)
{
	return InspectionValue::Object({{"kind", InspectionValue::Unsigned(static_cast<std::uint32_t>(source.kind))},
									{"id", InspectionValue::Unsigned(source.id)}});
}

static InspectionValue BodyValue(const BodyControls& body)
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
									{"root_pose", body.rootPose ? PoseValue(*body.rootPose) : InspectionValue::Null()},
									{"confidence", InspectionValue::Float(body.confidence)},
									{"preset", InspectionValue::Unsigned(static_cast<std::uint32_t>(body.preset))},
									{"controls", InspectionValue::Array(std::move(controls))},
									{"joints", InspectionValue::Array(std::move(joints))}});
}

static InspectionValue Configuration(const CommandConfiguration& configuration)
{
	std::vector<InspectionValue> controllers, trackers, outerFov;
	for (const auto& fov : configuration.views.outerFov)
		for (const auto angle : {fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown})
			outerFov.push_back(InspectionValue::Float(angle));
	for (std::size_t index = 0; index < configuration.tracking.controllers.size(); ++index)
		controllers.push_back(InspectionValue::Object(
			{{"pose", PoseValue(configuration.tracking.controllers[index].pose)},
			 {"aim", configuration.tracking.controllers[index].aim
						 ? PoseValue(*configuration.tracking.controllers[index].aim)
						 : InspectionValue{}},
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
			 {"flags", InspectionValue::Unsigned(tracker.poseFlags)},
			 {"source_path", InspectionValue::Text(tracker.sourcePath.View())},
			 {"joint_offset", tracker.jointOffset ? PoseValue(*tracker.jointOffset) : InspectionValue::Null()},
			 {"pose_override", tracker.poseOverride ? PoseValue(*tracker.poseOverride) : InspectionValue::Null()}}));
	return InspectionValue::Object(
		{{"head", PoseValue(configuration.tracking.head)},
		 {"eye_views", EyeViews(configuration.tracking.eyeViews)},
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

static InspectionValue EncodedCommand(const SimulationCommand& command)
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

static std::vector<InspectionValue> ConfigurationCommands(const CommandConfiguration& configuration,
														  std::uint32_t version)
{
	std::vector<InspectionValue> initial;
	const auto add = [&](SimulationCommand command) {
		if (const auto* body = std::get_if<SetBodyControls>(&command); body && body->observation && version < 4)
			throw std::invalid_argument("Body observations require recording schema 4");
		initial.push_back(EncodedCommand(command));
	};
	add(SetHeadPose{configuration.tracking.head});
	for (std::uint32_t hand = 0; hand < 2; ++hand)
	{
		add(SetControllerPose{hand, configuration.tracking.controllers[hand]});
		add(SetControllerControls{hand, configuration.input.controllers[hand]});
		add(SetControllerProfile{hand, configuration.input.preferredProfiles[hand]});
	}
	add(SetBodyControls{configuration.tracking.body, configuration.tracking.bodyObservation});
	add(SetGaze{configuration.tracking.gaze});
	if (version >= 3)
		add(SetTrackers{configuration.tracking.trackers});
	else
		for (std::uint32_t index = 0; index < configuration.tracking.trackers.size(); ++index)
			add(SetTracker{index, configuration.tracking.trackers[index]});
	add(SetViewSettings{configuration.views});
	if (version >= 3 || configuration.tracking.eyeViews)
		add(SetEyeViews{configuration.tracking.eyeViews});
	add(SetBodyProportions{configuration.proportions});
	add(SetReferenceSpaces{configuration.referenceSpaces});
	return initial;
}

static InspectionValue ReleaseSnapshot(const CommandRecordingOrigin& origin)
{
	std::vector<InspectionValue> scenarios;
	for (std::size_t index = 0; index < origin.scenarios.size(); ++index)
		if (origin.scenarios[index])
			scenarios.push_back(InspectionValue::Unsigned(index));
	return InspectionValue::Object(
		{{"schema", InspectionValue::Unsigned(1)},
		 {"frame", InspectionValue::Unsigned(origin.frame.value)},
		 {"runtime_time",
		  origin.runtimeTime ? InspectionValue::Signed(origin.runtimeTime->value.count()) : InspectionValue::Null()},
		 {"initial_commands", InspectionValue::Array(ConfigurationCommands(origin.configuration, 4))},
		 {"scenarios", InspectionValue::Array(std::move(scenarios))}});
}

InspectionValue SimulationOutcome(const CommandOutcome& outcome, bool includeCommands)
{
	std::vector<InspectionValue> commands;
	if (includeCommands)
		for (const auto& command : outcome.transaction.commands)
			commands.push_back(EncodedCommand(command));
	return InspectionValue::Object(
		{{"transaction_id", InspectionValue::Unsigned(outcome.transaction.id)},
		 {"release_state",
		  includeCommands && outcome.releaseState ? ReleaseSnapshot(*outcome.releaseState) : InspectionValue::Null()},
		 {"live_input",
		  outcome.liveInput
			  ? InspectionValue::Object(
					{{"connection", InspectionValue::Unsigned(outcome.liveInput->connection)},
					 {"sequence", InspectionValue::Unsigned(outcome.liveInput->sequence)},
					 {"clock_revision", InspectionValue::Unsigned(outcome.liveInput->clockRevision)},
					 {"observed_time", InspectionValue::Signed(outcome.liveInput->observedTime.value.count())},
					 {"target_time", InspectionValue::Signed(outcome.liveInput->targetTime.value.count())},
					 {"trackers_observed_time",
					  outcome.liveInput->trackersObservedTime
						  ? InspectionValue::Signed(outcome.liveInput->trackersObservedTime->value.count())
						  : InspectionValue::Null()},
					 {"controls_observed_time",
					  InspectionValue::Array(
						  {InspectionValue::Signed(outcome.liveInput->controlsObservedTime[0].value.count()),
						   InspectionValue::Signed(outcome.liveInput->controlsObservedTime[1].value.count())})}})
			  : InspectionValue::Null()},
		 {"source", SourceValue(outcome.transaction.source)},
		 {"phase", InspectionValue::Text(SimulationResultName(outcome.result))},
		 {"applied_frame", InspectionValue::Unsigned(outcome.appliedFrame.value)},
		 {"after_frame", InspectionValue::Boolean(outcome.afterFrame)},
		 {"applied_time", InspectionValue::Signed(outcome.appliedTime.value.count())},
		 {"applied_runtime_time", outcome.appliedRuntimeTime
									  ? InspectionValue::Signed(outcome.appliedRuntimeTime->value.count())
									  : InspectionValue::Null()},
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

InspectionValue SimulationState(const CommandSchedulerSnapshot& state, bool includeCommands,
								std::optional<bool> trackerInteraction, std::span<const std::uint64_t> boundTrackers,
								std::optional<std::array<std::string, 2>> controllerProfiles,
								std::optional<std::array<bool, 3>> bodyExtensions)
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
	if (controllerProfiles)
		fields.emplace_back("controller_profiles",
							InspectionValue::Array({InspectionValue::Text((*controllerProfiles)[0]),
													InspectionValue::Text((*controllerProfiles)[1])}));
	if (bodyExtensions)
		fields.emplace_back("body_extensions",
							InspectionValue::Object({{"fb", InspectionValue::Boolean((*bodyExtensions)[0])},
													 {"meta_full", InspectionValue::Boolean((*bodyExtensions)[1])},
													 {"bd", InspectionValue::Boolean((*bodyExtensions)[2])}}));
	if (trackerInteraction)
	{
		fields.emplace_back("tracker_interaction", InspectionValue::Boolean(*trackerInteraction));
		std::vector<InspectionValue> bound;
		for (const auto identity : boundTrackers)
			bound.push_back(InspectionValue::Unsigned(identity));
		fields.emplace_back("bound_trackers", InspectionValue::Array(std::move(bound)));
	}
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
	auto initial = ConfigurationCommands(recording.origin.configuration, recording.version);
	std::vector<InspectionValue> scenarios;
	for (std::size_t index = 0; index < recording.origin.scenarios.size(); ++index)
		if (recording.origin.scenarios[index])
			scenarios.push_back(InspectionValue::Unsigned(index));
	std::vector<InspectionValue> outcomes;
	for (const auto& outcome : recording.transactions)
	{
		for (const auto& command : outcome.transaction.commands)
			if (const auto* body = std::get_if<SetBodyControls>(&command);
				body && body->observation && recording.version < 4)
				throw std::invalid_argument("Body observations require recording schema 4");
		outcomes.push_back(SimulationOutcome(outcome));
	}
	return InspectionValue::Object(
		{{"version", InspectionValue::Unsigned(recording.version)},
		 {"recording_id", InspectionValue::Unsigned(recording.identity)},
		 {"drain", InspectionValue::Boolean(recording.drain)},
		 {"floor", InspectionValue::Unsigned(recording.floor)},
		 {"active", InspectionValue::Boolean(recording.active)},
		 {"overflow", InspectionValue::Boolean(recording.overflow)},
		 {"seed", InspectionValue::Unsigned(recording.origin.seed)},
		 {"origin_frame", InspectionValue::Unsigned(recording.origin.frame.value)},
		 {"origin_time", InspectionValue::Signed(recording.origin.time.value.count())},
		 {"origin_runtime_time", recording.origin.runtimeTime
									 ? InspectionValue::Signed(recording.origin.runtimeTime->value.count())
									 : InspectionValue::Null()},
		 {"initial", Configuration(recording.origin.configuration)},
		 {"initial_commands", InspectionValue::Array(std::move(initial))},
		 {"initial_scenarios", InspectionValue::Array(std::move(scenarios))},
		 {"total", InspectionValue::Unsigned(recording.total)},
		 {"transactions", InspectionValue::Array(std::move(outcomes))}});
}
} // namespace simxr::control
