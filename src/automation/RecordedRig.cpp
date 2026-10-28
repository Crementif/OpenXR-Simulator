#include "Pch.h"
#include "automation/Recording.h"
#include "automation/RigTypes.h"

#include "control/SimulationControl.h"
#include "simulation/BodyObservation.h"

#include <initializer_list>

namespace simxr::automation
{
static std::string Quote(std::string_view text)
{
	std::string result = "\"";
	for (const unsigned char character : text)
	{
		if (character == '"' || character == '\\')
		{
			result += '\\';
			result += static_cast<char>(character);
		}
		else if (character < 32 || character == 127)
		{
			result += '\\';
			result += static_cast<char>('0' + character / 100);
			result += static_cast<char>('0' + character / 10 % 10);
			result += static_cast<char>('0' + character % 10);
		}
		else
			result += static_cast<char>(character);
	}
	return result + '"';
}
static std::string Number(float value)
{
	if (!std::isfinite(value))
		throw std::invalid_argument("Recorded values must be finite");
	std::array<char, 48> buffer;
	const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
										 std::chars_format::general, std::numeric_limits<float>::max_digits10);
	if (converted.ec != std::errc{})
		throw std::runtime_error("Cannot format recorded value");
	return {buffer.data(), converted.ptr};
}
static std::string Vector(std::initializer_list<float> values)
{
	std::string result = "{";
	for (const auto value : values)
	{
		if (result.size() > 1)
			result += ", ";
		result += Number(value);
	}
	return result + '}';
}
class CommandTable
{
  public:
	explicit CommandTable(std::string_view kind = {})
	{
		if (!kind.empty())
			Text("kind", kind);
	}
	void Raw(std::string_view name, std::string value)
	{
		if (!m_fields.empty())
			m_fields += ", ";
		m_fields += name;
		m_fields += " = ";
		m_fields += value;
	}
	void Text(std::string_view name, std::string_view value)
	{
		Raw(name, Quote(value));
	}
	void Float(std::string_view name, float value)
	{
		Raw(name, Number(value));
	}
	void Integer(std::string_view name, std::uint64_t value)
	{
		Text(name, std::to_string(value));
	}
	void Boolean(std::string_view name, bool value)
	{
		Raw(name, value ? "true" : "false");
	}
	void Pose(const XrPosef& pose)
	{
		Raw("position", Vector({pose.position.x, pose.position.y, pose.position.z}));
		Raw("orientation", Vector({pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w}));
	}
	std::string Finish() const
	{
		return "{" + m_fields + "}";
	}

  private:
	std::string m_fields;
};
static constexpr std::array s_channelNames{"head",			 "left_pose",	"right_pose", "left_controls",
										   "right_controls", "body",		"gaze",		  "trackers",
										   "views",			 "proportions", "profiles",	  "session"};
static constexpr std::array s_scenarioNames{
	"head_untracked",	"head_unavailable",	 "body_untracked",	   "body_unavailable",		"gaze_untracked",
	"gaze_unavailable", "left_disconnected", "right_disconnected", "trackers_disconnected", "unfocused",
	"hidden",			"recenter",			 "session_loss"};
static std::string Format(const TakeControl& command)
{
	CommandTable table("take_control");
	table.Text("channel", s_channelNames.at(static_cast<std::size_t>(command.channel)));
	table.Boolean("replace", command.replace);
	return table.Finish();
}
static std::string Format(const ReleaseControl& command)
{
	CommandTable table("release_control");
	table.Text("channel", s_channelNames.at(static_cast<std::size_t>(command.channel)));
	return table.Finish();
}
static std::string Format(const SetHeadPose& command)
{
	CommandTable table("head_pose");
	table.Pose(command.value.pose);
	table.Integer("flags", command.value.flags);
	return table.Finish();
}
static std::string Format(const SetControllerPose& command)
{
	CommandTable table("controller_pose");
	table.Integer("hand", command.hand);
	table.Pose(command.value.pose.pose);
	table.Integer("flags", command.value.pose.flags);
	table.Boolean("connected", command.value.connected);
	if (command.value.aim)
	{
		CommandTable aim;
		aim.Pose(command.value.aim->pose);
		aim.Integer("flags", command.value.aim->flags);
		table.Raw("aim", aim.Finish());
	}
	return table.Finish();
}
static std::string Format(const SetControllerControls& command)
{
	CommandTable table("controller_controls");
	table.Integer("hand", command.hand);
	const auto& controls = command.value;
	table.Float("trigger", controls.trigger);
	table.Float("grip", controls.grip);
	table.Float("grip_force", controls.gripForce);
	table.Float("stick_x", controls.stick.x);
	table.Float("stick_y", controls.stick.y);
	table.Float("trackpad_x", controls.trackpad.x);
	table.Float("trackpad_y", controls.trackpad.y);
	table.Float("trackpad_force", controls.trackpadForce);
	table.Boolean("primary", controls.primary);
	table.Boolean("secondary", controls.secondary);
	table.Boolean("menu", controls.menu);
	table.Boolean("back", controls.back);
	table.Boolean("thumb_click", controls.thumbClick);
	table.Boolean("trackpad_click", controls.trackpadClick);
	table.Boolean("primary_touch", controls.primaryTouch);
	table.Boolean("secondary_touch", controls.secondaryTouch);
	table.Boolean("trigger_touch", controls.triggerTouch);
	if (!controls.physicalProfile.empty())
		table.Text("physical_profile", controls.physicalProfile);
	if (controls.triggerClick)
		table.Boolean("trigger_click", *controls.triggerClick);
	table.Boolean("thumb_touch", controls.thumbTouch);
	table.Boolean("trackpad_touch", controls.trackpadTouch);
	table.Boolean("thumbrest_touch", controls.thumbrestTouch);
	table.Boolean("connected", controls.connected);
	return table.Finish();
}
static std::string FormatObservation(const BodyObservation& source)
{
	const auto body = NormalizeBodyObservation(source);
	CommandTable table;
	table.Integer("joint_set", static_cast<std::uint32_t>(body.jointSet));
	table.Integer("requested_time", body.requestedTime.value.count());
	if (body.returnedTime)
		table.Integer("returned_time", body.returnedTime->value.count());
	table.Boolean("located", body.located);
	if (body.active)
		table.Boolean("active", *body.active);
	if (body.allJointPosesTracked)
		table.Boolean("all_joint_poses_tracked", *body.allJointPosesTracked);
	if (body.confidence)
		table.Float("confidence", *body.confidence);
	if (body.skeletonRevision)
		table.Integer("skeleton_revision", *body.skeletonRevision);
	std::string joints = "{";
	for (std::size_t index = 0; index < BodyJointCount; ++index)
		if (const auto& joint = body.joints[index])
		{
			CommandTable entry;
			entry.Integer("joint", index);
			entry.Pose(joint->pose);
			entry.Integer("flags", joint->flags);
			joints += entry.Finish() + ", ";
		}
	table.Raw("joints", joints + '}');
	if (body.skeleton)
	{
		std::string skeleton = "{";
		for (const auto& joint : *body.skeleton)
			if (joint)
			{
				CommandTable entry;
				entry.Integer("joint", static_cast<std::uint32_t>(joint->joint));
				if (joint->parent)
					entry.Integer("parent", static_cast<std::uint32_t>(*joint->parent));
				entry.Pose(joint->bindPose);
				skeleton += entry.Finish() + ", ";
			}
		table.Raw("skeleton", skeleton + '}');
	}
	return table.Finish();
}
static std::string Format(const SetBodyControls& command)
{
	if (command.observationOrigin && (!command.observation || command.observationOrigin->value.count() <= 0))
		throw std::invalid_argument("Body replay timing requires an observation and positive anchor");
	CommandTable table("body");
	if (command.observationOrigin)
		table.Integer("observation_origin", command.observationOrigin->value.count());
	if (command.observation)
		table.Raw("observation", FormatObservation(*command.observation));
	const auto& body = command.value;
	table.Boolean("active", body.active);
	if (body.rootPose)
	{
		CommandTable root;
		root.Pose(*body.rootPose);
		table.Raw("root_pose", root.Finish());
	}
	table.Float("confidence", body.confidence);
	table.Integer("preset", static_cast<std::uint32_t>(body.preset));
	std::string controls = "{";
	for (const auto& control : body.controls)
	{
		CommandTable entry;
		entry.Boolean("tracked", control.tracked);
		if (control.offset)
			entry.Raw("offset", Vector({control.offset->x, control.offset->y, control.offset->z}));
		controls += entry.Finish() + ", ";
	}
	table.Raw("controls", controls + '}');
	std::string joints = "{";
	for (std::size_t index = 0; index < body.joints.size(); ++index)
		if (const auto& joint = body.joints[index])
		{
			CommandTable entry;
			entry.Integer("joint", index);
			entry.Boolean("tracked", joint->tracked);
			if (joint->pose)
				entry.Pose(*joint->pose);
			joints += entry.Finish() + ", ";
		}
	table.Raw("joints", joints + '}');
	return table.Finish();
}
static std::string Format(const SetGaze& command)
{
	CommandTable table("gaze");
	table.Float("yaw", command.value.yaw);
	table.Float("pitch", command.value.pitch);
	table.Boolean("available", command.value.available);
	table.Boolean("tracked", command.value.tracked);
	return table.Finish();
}
static std::string Format(const SetTracker& command)
{
	CommandTable table("tracker");
	table.Integer("index", command.index);
	table.Integer("identity", command.value.identity.value);
	if (!command.value.sourcePath.View().empty())
		table.Text("source_path", command.value.sourcePath.View());
	if (command.value.poseFlags != TrackedPoseFlags)
		table.Integer("flags", command.value.poseFlags);
	table.Integer("role", static_cast<std::uint32_t>(command.value.role));
	table.Boolean("connected", command.value.connected);
	table.Boolean("tracked", command.value.tracked);
	if (command.value.anchor)
		table.Integer("anchor", static_cast<std::uint32_t>(*command.value.anchor));
	if (command.value.jointOffset)
	{
		CommandTable offset;
		offset.Pose(*command.value.jointOffset);
		table.Raw("joint_offset", offset.Finish());
	}
	if (command.value.poseOverride)
		table.Pose(*command.value.poseOverride);
	return table.Finish();
}
static std::string Format(const SetTrackers& command)
{
	CommandTable table("trackers");
	std::string entries = "{";
	for (std::size_t index = 0; index < command.value.size(); ++index)
		entries += Format(SetTracker{static_cast<std::uint32_t>(index), command.value[index]}) + ", ";
	table.Raw("devices", entries + '}');
	return table.Finish();
}
static std::string Format(const SetEyeViews& command)
{
	CommandTable table("eye_views");
	table.Boolean("enabled", command.value.has_value());
	if (command.value)
	{
		table.Integer("space_revision", command.value->spaceRevision);
		for (std::size_t eye = 0; eye < 2; ++eye)
		{
			CommandTable value;
			value.Pose(command.value->poses[eye].pose);
			value.Integer("flags", command.value->poses[eye].flags);
			const auto& fov = command.value->fovs[eye];
			value.Raw("fov", Vector({fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown}));
			table.Raw(eye == 0 ? "left" : "right", value.Finish());
		}
	}
	return table.Finish();
}
static std::string Format(const SetViewSettings& command)
{
	CommandTable table("views");
	const auto& views = command.value;
	table.Float("ipd", views.ipd);
	table.Float("inset_half_angle", views.insetHalfAngle);
	table.Integer("outer_width", views.outerWidth);
	table.Integer("outer_height", views.outerHeight);
	table.Integer("inset_width", views.insetWidth);
	table.Integer("inset_height", views.insetHeight);
	const auto& left = views.outerFov[0];
	const auto& right = views.outerFov[1];
	table.Raw("outer_fov", Vector({left.angleLeft, left.angleRight, left.angleUp, left.angleDown, right.angleLeft,
								   right.angleRight, right.angleUp, right.angleDown}));
	return table.Finish();
}
static std::string Format(const SetBodyProportions& command)
{
	CommandTable table("proportions");
	table.Float("height", command.value.height);
	table.Float("shoulder_width", command.value.shoulderWidth);
	table.Float("hip_width", command.value.hipWidth);
	return table.Finish();
}
static std::string Format(const SetControllerProfile& command)
{
	CommandTable table("controller_profile");
	table.Integer("hand", command.hand);
	table.Text("path", command.path);
	return table.Finish();
}
static std::string Format(const SetScenario& command)
{
	CommandTable table("scenario");
	table.Text("scenario", s_scenarioNames.at(static_cast<std::size_t>(command.scenario)));
	table.Boolean("enabled", command.enabled);
	return table.Finish();
}
static std::string Format(const SetReferenceSpaces& command)
{
	CommandTable table("reference_spaces");
	const auto& local = command.value.local;
	const auto& stage = command.value.stage;
	table.Raw("local_orientation",
			  Vector({local.orientation.x, local.orientation.y, local.orientation.z, local.orientation.w}));
	table.Raw("local_position", Vector({local.position.x, local.position.y, local.position.z}));
	table.Raw("stage_orientation",
			  Vector({stage.orientation.x, stage.orientation.y, stage.orientation.z, stage.orientation.w}));
	table.Raw("stage_position", Vector({stage.position.x, stage.position.y, stage.position.z}));
	table.Boolean("bounds_available", command.value.stageBounds.has_value());
	const auto bounds = command.value.stageBounds.value_or(XrExtent2Df{});
	table.Raw("stage_bounds", Vector({bounds.width, bounds.height}));
	return table.Finish();
}

std::string EditableCommand(const SimulationCommand& command)
{
	return std::visit(
		[](const auto& value)
		{
			return Format(value);
		},
		command);
}
SimulationCommand ReplayCommand(SimulationCommand command, std::optional<RuntimeTime> origin)
{
	if (auto* body = std::get_if<SetBodyControls>(&command); body && body->observation)
	{
		if (!origin || origin->value.count() <= 0)
			throw std::invalid_argument("Observed body replay requires its recorded runtime boundary");
		if (body->observationOrigin)
			throw std::invalid_argument("Recorded body command must contain resolved timestamps");
		body->observationOrigin = origin;
	}
	return command;
}
std::vector<SimulationCommand> ReplayInitialCommands(const InputRecording& recording)
{
	if (recording.initial.size() + s_channelNames.size() + s_scenarioNames.size() > 64)
		throw std::invalid_argument("Recording initialization exceeds 64 commands");
	if (recording.initialScenarios[static_cast<std::size_t>(FailureScenario::SessionLoss)])
		throw std::invalid_argument("A recording begun after session loss has no replayable live-session origin");
	if (std::count_if(recording.initial.begin(), recording.initial.end(),
					  [](const auto& command)
					  {
						  return std::holds_alternative<SetReferenceSpaces>(command);
					  }) != 1)
		throw std::invalid_argument("Recording requires one exact reference-space origin");
	for (const auto& command : recording.initial)
	{
		if (const auto* body = std::get_if<SetBodyControls>(&command); body && body->observation && recording.version < 4)
			throw std::invalid_argument("Body observations require recording schema 4");
		if (recording.version < 3 && std::holds_alternative<SetTrackers>(command))
			throw std::invalid_argument("Tracker collections require recording schema 3");
		if (const auto* spaces = std::get_if<SetReferenceSpaces>(&command))
		{
			auto normalized = spaces->value;
			if (!NormalizeReferenceSpaces(normalized))
				throw std::invalid_argument("Recording reference-space origin is invalid");
		}
		if (std::holds_alternative<TakeControl>(command) || std::holds_alternative<ReleaseControl>(command) ||
			std::holds_alternative<SetScenario>(command))
			throw std::invalid_argument("Recording initial configuration cannot contain ownership or event commands");
	}
	std::vector<SimulationCommand> commands;
	commands.reserve(s_channelNames.size() + recording.initial.size() + s_scenarioNames.size());
	for (std::size_t channel = 0; channel < s_channelNames.size(); ++channel)
		commands.push_back(TakeControl{static_cast<ControlChannel>(channel)});
	for (const auto& command : recording.initial)
		commands.push_back(ReplayCommand(command, recording.originRuntimeTime));
	for (std::size_t index = 0; index < s_scenarioNames.size(); ++index)
	{
		const auto scenario = static_cast<FailureScenario>(index);
		const bool enabled = scenario != FailureScenario::Recenter && scenario != FailureScenario::SessionLoss &&
							 recording.initialScenarios[index];
		commands.push_back(SetScenario{scenario, enabled});
	}
	return commands;
}

std::string EditableRig(const InputRecording& recording)
{
	if ((recording.version < 1 || recording.version > 4) || recording.active || recording.overflow ||
		recording.total != recording.transactions.size() || recording.total > 1024 ||
		recording.initial.size() + s_channelNames.size() + s_scenarioNames.size() > 64)
		throw std::invalid_argument("Export requires a stopped, complete supported recording");
	const auto initialCommands = ReplayInitialCommands(recording);
	std::string script;
	const auto line = [&](std::string value)
	{
		if (value.size() >= MaxRigSourceBytes - script.size())
			throw std::length_error("Editable rig exceeds the runner's 64-MiB source limit");
		script += value + '\n';
	};
	line("-- OpenXR Simulator recorded inputs, schema " + std::to_string(recording.version) +
		 ". Edit the command tables below.");
	line("-- Launch simxr_runner with --seed " + std::to_string(recording.seed));
	line("assert(simxr.seed == " + Quote(std::to_string(recording.seed)) + ", \"Recording seed differs\")");
	line("simxr.pause()");
	line("local baseFrame = simxr.frame_add(simxr.state().frame_id or \"0\", \"1\")");
	line("local function presentThrough(appliedFrame)");
	line("    repeat");
	line("        local capture, progress, sourceFrame = simxr.step_display()");
	line("        if capture then simxr.release_capture(capture) end");
	line("    until sourceFrame and not simxr.frame_before(sourceFrame, appliedFrame)");
	line("end");
	line("local function advance(target)");
	line("    while simxr.frame_before(simxr.state().frame_id or \"0\", target) do");
	line("        local capture = simxr.step()");
	line("        if capture then simxr.release_capture(capture) end");
	line("        local appliedFrame = simxr.state().frame_id or \"0\"");
	line("        presentThrough(appliedFrame)");
	line("    end");
	line("end");
	line("local initialCommands = {");
	line("    -- Restore persistent failures; do not repeat the origin's consumed recenter event.");
	for (const auto& command : initialCommands)
		line("    " + EditableCommand(command) + ",");
	line("}");
	line("local initial = simxr.schedule({frame = baseFrame, commands = initialCommands})");
	line("advance(baseFrame)");
	line("simxr.await_transaction(initial)");
	std::map<std::uint64_t, std::vector<const RecordedTransaction*>> frames;
	for (const auto& transaction : recording.transactions)
	{
		for (const auto& command : transaction.commands)
		{
			if (const auto* body = std::get_if<SetBodyControls>(&command); body && body->observation && recording.version < 4)
				throw std::invalid_argument("Body observations require recording schema 4");
			if (recording.version < 3 && std::holds_alternative<SetTrackers>(command))
				throw std::invalid_argument("Tracker collections require recording schema 3");
		}
		if (transaction.commands.empty() || transaction.commands.size() > 64)
			throw std::invalid_argument("Recorded transaction has an invalid command count");
		const bool released = transaction.phase == "released";
		if ((transaction.afterFrame && !released) ||
			(released && (recording.version < 2 || transaction.id != 0 ||
						  !std::all_of(
							  transaction.commands.begin(), transaction.commands.end(),
							  [](const auto& command)
							  {
								  return std::holds_alternative<ReleaseControl>(command);
							  }))))
			throw std::invalid_argument("Malformed recorded release event");
		if (transaction.phase != "applied" && !released)
		{
			constexpr std::array phases{"queued",  "late",	   "rescheduled", "conflict", "cancelled",
										"invalid", "capacity", "closed",	  "not_found"};
			if (std::find(phases.begin(), phases.end(), transaction.phase) == phases.end())
				throw std::invalid_argument("Unknown recorded transaction phase");
			line("-- Observed transaction " + std::to_string(transaction.id) + ": " + transaction.phase);
			continue;
		}
		if (transaction.frame < recording.originFrame ||
			(transaction.frame == recording.originFrame && !transaction.afterFrame))
			throw std::invalid_argument("Applied transaction precedes recording origin");
		auto relative = transaction.frame.value - recording.originFrame.value;
		if (transaction.afterFrame)
		{
			if (relative == std::numeric_limits<std::uint64_t>::max())
				throw std::overflow_error("Release event exceeds replay frame range");
			++relative;
		}
		frames[relative].push_back(&transaction);
	}
	for (const auto& [relative, transactions] : frames)
	{
		line("do -- recorded frame +" + std::to_string(relative));
		line("    local target = simxr.frame_add(baseFrame, " + Quote(std::to_string(relative)) + ")");
		line("    local pending = {}");
		for (const auto* transaction : transactions)
		{
			line("    -- Original frame " + std::to_string(transaction->frame.value) + "; transaction " +
				 std::to_string(transaction->id) + "; after_frame " + (transaction->afterFrame ? "true" : "false"));
			if (const auto& live = transaction->liveInput)
			{
				line("    -- Physical input: connection " + std::to_string(live->connection) + "; sequence " +
					 std::to_string(live->sequence) + "; clock_revision " + std::to_string(live->clockRevision));
				line("    -- Original runtime ns: observed " + std::to_string(live->observedTime.value.count()) +
					 "; target " + std::to_string(live->targetTime.value.count()) + "; controls " +
					 std::to_string(live->controlsObservedTime[0].value.count()) + ", " +
					 std::to_string(live->controlsObservedTime[1].value.count()));
				if (live->trackersObservedTime)
					line("    -- Tracker observation runtime ns: " +
						 std::to_string(live->trackersObservedTime->value.count()));
			}
			line("    table.insert(pending, simxr.schedule({frame = target, commands = {");
			for (const auto& command : transaction->commands)
				line("        " + EditableCommand(ReplayCommand(command, transaction->runtimeTime)) + ",");
			line("    }}))");
		}
		line("    advance(target)");
		line("    for _, id in pending do simxr.await_transaction(id) end");
		line("end");
	}
	line("simxr.release_inputs()");
	line("simxr.resume()");
	line("simxr.checkpoint(\"recording replay\", true)");
	return script;
}
} // namespace simxr::automation
