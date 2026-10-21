#include "automation/Recording.h"
#include "control/SimulationControl.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>

namespace simxr::automation
{
namespace
{
std::string Quote(std::string_view text)
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
std::string Number(float value)
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
std::string Vector(std::initializer_list<float> values)
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
class Table
{
  public:
	explicit Table(std::string_view kind = {})
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
constexpr std::array ChannelNames{"head", "left_pose", "right_pose", "left_controls", "right_controls", "body",
								  "gaze", "trackers",  "views",		 "proportions",	  "profiles",		"session"};
constexpr std::array ScenarioNames{
	"head_untracked",	"head_unavailable",	 "body_untracked",	   "body_unavailable",		"gaze_untracked",
	"gaze_unavailable", "left_disconnected", "right_disconnected", "trackers_disconnected", "unfocused",
	"hidden",			"recenter",			 "session_loss"};
std::string Format(const TakeControl& command)
{
	Table table("take_control");
	table.Text("channel", ChannelNames.at(static_cast<std::size_t>(command.channel)));
	table.Boolean("replace", command.replace);
	return table.Finish();
}
std::string Format(const ReleaseControl& command)
{
	Table table("release_control");
	table.Text("channel", ChannelNames.at(static_cast<std::size_t>(command.channel)));
	return table.Finish();
}
std::string Format(const SetHeadPose& command)
{
	Table table("head_pose");
	table.Pose(command.value.pose);
	table.Integer("flags", command.value.flags);
	return table.Finish();
}
std::string Format(const SetControllerPose& command)
{
	Table table("controller_pose");
	table.Integer("hand", command.hand);
	table.Pose(command.value.pose.pose);
	table.Integer("flags", command.value.pose.flags);
	table.Boolean("connected", command.value.connected);
	return table.Finish();
}
std::string Format(const SetControllerControls& command)
{
	Table table("controller_controls");
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
	table.Boolean("thumb_touch", controls.thumbTouch);
	table.Boolean("trackpad_touch", controls.trackpadTouch);
	table.Boolean("thumbrest_touch", controls.thumbrestTouch);
	table.Boolean("connected", controls.connected);
	return table.Finish();
}
std::string Format(const SetBodyControls& command)
{
	Table table("body");
	const auto& body = command.value;
	table.Boolean("active", body.active);
	if (body.rootPose) { Table root; root.Pose(*body.rootPose); table.Raw("root_pose", root.Finish()); }
	table.Float("confidence", body.confidence);
	table.Integer("preset", static_cast<std::uint32_t>(body.preset));
	std::string controls = "{";
	for (const auto& control : body.controls)
	{
		Table entry;
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
			Table entry;
			entry.Integer("joint", index);
			entry.Boolean("tracked", joint->tracked);
			if (joint->pose)
				entry.Pose(*joint->pose);
			joints += entry.Finish() + ", ";
		}
	table.Raw("joints", joints + '}');
	return table.Finish();
}
std::string Format(const SetGaze& command)
{
	Table table("gaze");
	table.Float("yaw", command.value.yaw);
	table.Float("pitch", command.value.pitch);
	table.Boolean("available", command.value.available);
	table.Boolean("tracked", command.value.tracked);
	return table.Finish();
}
std::string Format(const SetTracker& command)
{
	Table table("tracker");
	table.Integer("index", command.index);
	table.Integer("identity", command.value.identity.value);
	table.Integer("role", static_cast<std::uint32_t>(command.value.role));
	table.Boolean("connected", command.value.connected);
	table.Boolean("tracked", command.value.tracked);
	if (command.value.anchor)
		table.Integer("anchor", static_cast<std::uint32_t>(*command.value.anchor));
	if (command.value.poseOverride)
		table.Pose(*command.value.poseOverride);
	return table.Finish();
}
std::string Format(const SetViewSettings& command)
{
	Table table("views");
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
std::string Format(const SetBodyProportions& command)
{
	Table table("proportions");
	table.Float("height", command.value.height);
	table.Float("shoulder_width", command.value.shoulderWidth);
	table.Float("hip_width", command.value.hipWidth);
	return table.Finish();
}
std::string Format(const SetControllerProfile& command)
{
	Table table("controller_profile");
	table.Integer("hand", command.hand);
	table.Text("path", command.path);
	return table.Finish();
}
std::string Format(const SetScenario& command)
{
	Table table("scenario");
	table.Text("scenario", ScenarioNames.at(static_cast<std::size_t>(command.scenario)));
	table.Boolean("enabled", command.enabled);
	return table.Finish();
}
std::string Format(const SetReferenceSpaces& command)
{
	Table table("reference_spaces");
	const auto& local = command.value.local;
	const auto& stage = command.value.stage;
	table.Raw("local_orientation", Vector({local.orientation.x, local.orientation.y, local.orientation.z, local.orientation.w}));
	table.Raw("local_position", Vector({local.position.x, local.position.y, local.position.z}));
	table.Raw("stage_orientation", Vector({stage.orientation.x, stage.orientation.y, stage.orientation.z, stage.orientation.w}));
	table.Raw("stage_position", Vector({stage.position.x, stage.position.y, stage.position.z}));
	table.Boolean("bounds_available", command.value.stageBounds.has_value());
	const auto bounds = command.value.stageBounds.value_or(XrExtent2Df{});
	table.Raw("stage_bounds", Vector({bounds.width, bounds.height}));
	return table.Finish();
}
} // namespace
std::string EditableCommand(const SimulationCommand& command)
{
	return std::visit(
		[](const auto& value)
		{
			return Format(value);
		},
		command);
}
std::vector<SimulationCommand> ReplayInitialCommands(const InputRecording& recording)
{
	if (recording.initial.size() + ChannelNames.size() + ScenarioNames.size() > 64)
		throw std::invalid_argument("Recording initialization exceeds 64 commands");
	if (recording.initialScenarios[static_cast<std::size_t>(FailureScenario::SessionLoss)])
		throw std::invalid_argument("A recording begun after session loss has no replayable live-session origin");
	if (std::count_if(recording.initial.begin(), recording.initial.end(),
		[](const auto& command) { return std::holds_alternative<SetReferenceSpaces>(command); }) != 1)
		throw std::invalid_argument("Recording requires one exact reference-space origin");
	for (const auto& command : recording.initial)
	{
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
	commands.reserve(ChannelNames.size() + recording.initial.size() + ScenarioNames.size());
	for (std::size_t channel = 0; channel < ChannelNames.size(); ++channel)
		commands.push_back(TakeControl{static_cast<ControlChannel>(channel)});
	commands.insert(commands.end(), recording.initial.begin(), recording.initial.end());
	for (std::size_t index = 0; index < ScenarioNames.size(); ++index)
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
	if (recording.version != 1 || recording.active || recording.overflow ||
		recording.total != recording.transactions.size() || recording.total > 1024 ||
		recording.initial.size() + ChannelNames.size() + ScenarioNames.size() > 64)
		throw std::invalid_argument("Export requires a stopped, complete version 1 recording");
	const auto initialCommands = ReplayInitialCommands(recording);
	std::string script;
	const auto line = [&](std::string value)
	{
		if (value.size() >= control::MaxPayload - script.size())
			throw std::length_error("Editable rig exceeds the runner's one-MiB source limit");
		script += value + '\n';
	};
	line("-- OpenXR Simulator recorded inputs, schema 1. Edit the command tables below.");
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
		if (transaction.commands.empty() || transaction.commands.size() > 64)
			throw std::invalid_argument("Recorded transaction has an invalid command count");
		if (transaction.phase != "applied")
		{
			constexpr std::array phases{"queued",  "late",	   "rescheduled", "conflict", "cancelled",
										"invalid", "capacity", "closed",	  "not_found"};
			if (std::find(phases.begin(), phases.end(), transaction.phase) == phases.end())
				throw std::invalid_argument("Unknown recorded transaction phase");
			line("-- Observed transaction " + std::to_string(transaction.id) + ": " + transaction.phase);
			continue;
		}
		if (transaction.frame <= recording.originFrame)
			throw std::invalid_argument("Applied transaction precedes recording origin");
		frames[transaction.frame.value - recording.originFrame.value].push_back(&transaction);
	}
	for (const auto& [relative, transactions] : frames)
	{
		line("do -- recorded frame +" + std::to_string(relative));
		line("    local target = simxr.frame_add(baseFrame, " + Quote(std::to_string(relative)) + ")");
		line("    local pending = {}");
		for (const auto* transaction : transactions)
		{
			line("    table.insert(pending, simxr.schedule({frame = target, commands = {");
			for (const auto& command : transaction->commands)
				line("        " + EditableCommand(command) + ",");
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
