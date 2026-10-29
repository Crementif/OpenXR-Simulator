#include "app/mcp/McpPch.h"
#include "app/mcp/McpSimulation.h"
#include "control/SimulationControl.h"
#include "simulation/BodyObservation.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;
namespace wire = control;

static void Keys(const Json& object, std::initializer_list<std::string_view> allowed)
{
	if (!object.is_object())
		throw std::invalid_argument("Each simulation command must be an object");
	for (auto it = object.begin(); it != object.end(); ++it)
		if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end())
			throw std::invalid_argument("Unknown simulation command argument: " + it.key());
}

static std::string Text(const Json& object, const char* key)
{
	const auto it = object.find(key);
	if (it == object.end() || !it->is_string())
		throw std::invalid_argument(std::string(key) + " must be a string");
	const auto value = it->get<std::string>();
	if (value.empty() || value.size() > 512 || value.find('\0') != std::string::npos)
		throw std::invalid_argument(std::string("Invalid ") + key);
	return value;
}

static bool Boolean(const Json& object, const char* key, bool fallback)
{
	const auto it = object.find(key);
	if (it == object.end())
		return fallback;
	if (!it->is_boolean())
		throw std::invalid_argument(std::string(key) + " must be boolean");
	return it->get<bool>();
}

static std::uint32_t Integer(const Json& object, const char* key, std::uint32_t fallback, std::uint32_t maximum)
{
	const auto it = object.find(key);
	if (it == object.end())
		return fallback;
	if (!it->is_number_integer() || *it < 0 || *it > maximum)
		throw std::invalid_argument(std::string(key) + " is out of range");
	return it->get<std::uint32_t>();
}

static std::uint32_t Device(const Json& object, const char* key, std::uint32_t maximum)
{
	if (!object.contains(key))
		throw std::invalid_argument(std::string(key) + " is required");
	return Integer(object, key, 0, maximum);
}

static std::uint64_t WideInteger(const Json& object, const char* key, std::uint64_t maximum, bool allowZero = true)
{
	const auto it = object.find(key);
	if (it == object.end())
		throw std::invalid_argument(std::string(key) + " is required");
	std::uint64_t value = 0;
	if (it->is_string())
	{
		const auto text = it->get<std::string>();
		if (text.empty() || (text.size() > 1 && text.front() == '0'))
			throw std::invalid_argument(std::string(key) + " requires a canonical decimal integer");
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
			throw std::invalid_argument(std::string(key) + " requires a canonical decimal integer");
	}
	else if (it->is_number_integer() && *it >= 0 && *it <= ((1ULL << 53) - 1))
		value = it->get<std::uint64_t>();
	else
		throw std::invalid_argument(std::string(key) + " requires a decimal string or safe integer");
	if (value > maximum || (!allowZero && value == 0))
		throw std::invalid_argument(std::string(key) + " is out of range");
	return value;
}

static float Float(const Json& value)
{
	if (!value.is_number())
		throw std::invalid_argument("Simulation values must be finite numbers");
	const auto number = value.get<double>();
	if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
		throw std::invalid_argument("Simulation values must fit a finite float");
	return static_cast<float>(number);
}

static float Float(const Json& object, const char* key, float fallback)
{
	const auto it = object.find(key);
	return it == object.end() ? fallback : Float(*it);
}

static float Axis(const Json& object, const char* key, float minimum = 0)
{
	const auto value = Float(object, key, 0);
	if (value < minimum || value > 1)
		throw std::invalid_argument(std::string(key) + " is outside its input range");
	return value;
}

template<std::size_t Size>
static std::array<float, Size> Vector(const Json& object, const char* key, std::array<float, Size> fallback)
{
	const auto it = object.find(key);
	if (it == object.end())
		return fallback;
	if (!it->is_array() || it->size() != Size)
		throw std::invalid_argument(std::string(key) + " has invalid dimensions");
	for (std::size_t index = 0; index < Size; ++index)
		fallback[index] = Float((*it)[index]);
	return fallback;
}

static XrPosef Pose(const Json& object, const char* position = "position", const char* orientation = "orientation")
{
	const auto p = Vector<3>(object, position, {0, 0, 0});
	const auto q = Vector<4>(object, orientation, {0, 0, 0, 1});
	return {{q[0], q[1], q[2], q[3]}, {p[0], p[1], p[2]}};
}

static PoseSample Sample(const Json& object)
{
	PoseSample result;
	result.pose = Pose(object);
	result.flags = Integer(object, "flags", 15, 15);
	return result;
}

static std::uint32_t Joint(const Json& object, const char* key)
{
	if (!object.contains(key))
		throw std::invalid_argument(std::string(key) + " is required");
	return Integer(object, key, 0, BodyJointCount - 1);
}

static RuntimeTime BodyTime(const Json& object, const char* key, bool allowZero = false)
{
	return RuntimeTime{Duration{static_cast<std::int64_t>(WideInteger(object, key, INT64_MAX, allowZero))}};
}

static BodyObservation Observation(const Json& object)
{
	Keys(object, {"joint_set", "requested_time", "returned_time", "located", "active", "all_joint_poses_tracked",
				  "confidence", "skeleton_revision", "joints", "skeleton"});
	if (!object.contains("joint_set") || !object.contains("located"))
		throw std::invalid_argument("Body observation requires joint_set and located");
	BodyObservation result;
	result.jointSet = static_cast<BodyJointSet>(Integer(object, "joint_set", 0, 3));
	result.requestedTime = BodyTime(object, "requested_time");
	result.located = Boolean(object, "located", false);
	if (object.contains("returned_time"))
		result.returnedTime = BodyTime(object, "returned_time", true);
	if (object.contains("active"))
		result.active = Boolean(object, "active", false);
	if (object.contains("all_joint_poses_tracked"))
		result.allJointPosesTracked = Boolean(object, "all_joint_poses_tracked", false);
	if (object.contains("confidence"))
		result.confidence = Float(object.at("confidence"));
	if (object.contains("skeleton_revision"))
		result.skeletonRevision = Integer(object, "skeleton_revision", 0, UINT32_MAX);
	for (const bool skeleton : {false, true})
	{
		const auto it = object.find(skeleton ? "skeleton" : "joints");
		if (skeleton && it == object.end())
			continue;
		if (it == object.end() || !it->is_array() || it->size() > BodyJointCount)
			throw std::invalid_argument("Observed body joints require a bounded array");
		if (skeleton)
			result.skeleton.emplace();
		for (const auto& entry : *it)
		{
			if (skeleton)
				Keys(entry, {"joint", "position", "orientation", "parent"});
			else
				Keys(entry, {"joint", "position", "orientation", "flags"});
			if (!entry.contains("position") || !entry.contains("orientation"))
				throw std::invalid_argument("Observed body joint requires a complete pose");
			const auto joint = Joint(entry, "joint");
			if (skeleton)
			{
				auto& value = (*result.skeleton)[joint];
				if (value)
					throw std::invalid_argument("Duplicate skeleton joint");
				value = SkeletonJoint{static_cast<BodyJoint>(joint), {}, Pose(entry)};
				if (entry.contains("parent"))
					value->parent = static_cast<BodyJoint>(Joint(entry, "parent"));
			}
			else
			{
				auto& value = result.joints[joint];
				if (value || !entry.contains("flags"))
					throw std::invalid_argument("Duplicate body joint or missing flags");
				value = ObservedBodyJoint{Pose(entry), Integer(entry, "flags", 0, 15)};
			}
		}
	}
	return NormalizeBodyObservation(std::move(result));
}

static SetBodyControls Body(const Json& object)
{
	Keys(object, {"kind", "active", "confidence", "preset", "root_pose", "controls", "joints", "observation",
				  "observation_origin"});
	SetBodyControls result;
	result.value.active = Boolean(object, "active", true);
	result.value.confidence = Float(object, "confidence", 1);
	result.value.preset = static_cast<BodyPreset>(Integer(object, "preset", 0, 2));
	if (const auto root = object.find("root_pose"); root != object.end() && !root->is_null())
	{
		Keys(*root, {"position", "orientation"});
		result.value.rootPose = Pose(*root);
	}
	if (const auto controls = object.find("controls"); controls != object.end())
	{
		if (!controls->is_array() || controls->size() != result.value.controls.size())
			throw std::invalid_argument("Body controls require five entries");
		for (std::size_t index = 0; index < controls->size(); ++index)
		{
			const auto& entry = (*controls)[index];
			Keys(entry, {"tracked", "offset"});
			auto& value = result.value.controls[index];
			value.tracked = Boolean(entry, "tracked", true);
			if (entry.contains("offset") && !entry["offset"].is_null())
			{
				const auto offset = Vector<3>(entry, "offset", {});
				value.offset = XrVector3f{offset[0], offset[1], offset[2]};
			}
		}
	}
	if (const auto joints = object.find("joints"); joints != object.end())
	{
		if (!joints->is_array() || joints->size() > BodyJointCount)
			throw std::invalid_argument("Body joint overrides require a bounded array");
		for (const auto& entry : *joints)
		{
			Keys(entry, {"joint", "tracked", "position", "orientation"});
			auto& value = result.value.joints[Joint(entry, "joint")];
			if (value)
				throw std::invalid_argument("Duplicate body joint override");
			value = BodyJointOverride{};
			value->tracked = Boolean(entry, "tracked", true);
			if (entry.contains("position") || entry.contains("orientation"))
				value->pose = Pose(entry);
		}
	}
	if (object.contains("observation"))
		result.observation = Observation(object.at("observation"));
	if (object.contains("observation_origin"))
	{
		if (!result.observation)
			throw std::invalid_argument("Body replay timing requires an observation");
		result.observationOrigin = BodyTime(object, "observation_origin");
	}
	return result;
}

static TrackerDevice Tracker(const Json& object)
{
	Keys(object, {"kind", "index", "identity", "role", "connected", "tracked", "anchor", "position", "orientation",
				  "joint_offset", "flags", "source_path"});
	TrackerDevice result{{WideInteger(object, "identity", UINT64_MAX, false)},
						 static_cast<TrackerRole>(Integer(object, "role", 4, 17)),
						 Boolean(object, "connected", true),
						 Boolean(object, "tracked", true)};
	result.poseFlags = Integer(object, "flags", 15, 15);
	if (const auto source = object.find("source_path"); source != object.end())
	{
		if (!source->is_string())
			throw std::invalid_argument("source_path must be a string");
		result.sourcePath = TrackerSourcePath{source->get<std::string>()};
	}
	if (object.contains("anchor") && !object["anchor"].is_null())
		result.anchor = static_cast<BodyJoint>(Joint(object, "anchor"));
	if (object.contains("position") || object.contains("orientation"))
		result.poseOverride = Pose(object);
	if (const auto offset = object.find("joint_offset"); offset != object.end() && !offset->is_null())
	{
		Keys(*offset, {"position", "orientation"});
		result.jointOffset = Pose(*offset);
	}
	if (result.poseOverride && result.jointOffset)
		throw std::invalid_argument("Tracker cannot combine world and joint-relative poses");
	return result;
}

static ControlChannel Channel(const Json& object)
{
	static constexpr std::array names{"head", "left_pose", "right_pose", "left_controls", "right_controls", "body",
									  "gaze", "trackers",  "views",		 "proportions",	  "profiles",		"session"};
	const auto it = object.find("channel");
	if (it == object.end())
		throw std::invalid_argument("channel is required");
	if (it->is_string())
	{
		const auto name = it->get<std::string>();
		const auto found = std::find(names.begin(), names.end(), name);
		if (found == names.end())
			throw std::invalid_argument("Unknown control channel");
		return static_cast<ControlChannel>(found - names.begin());
	}
	return static_cast<ControlChannel>(Integer(object, "channel", UINT32_MAX, names.size() - 1));
}

static SimulationCommand Canonical(const Json& object)
{
	Keys(object, {"encoding", "data"});
	if (Text(object, "encoding") != "simxr-command-v1-hex")
		throw std::invalid_argument("Unsupported canonical command encoding");
	const auto it = object.find("data");
	if (it == object.end() || !it->is_string())
		throw std::invalid_argument("Canonical command data must be hex");
	const auto hex = it->get<std::string>();
	if (hex.empty() || hex.size() > 262144 || hex.size() % 2)
		throw std::invalid_argument("Invalid canonical command size");
	const auto digit = [](char value) -> unsigned {
		if (value >= '0' && value <= '9')
			return value - '0';
		if (value >= 'a' && value <= 'f')
			return value - 'a' + 10;
		throw std::invalid_argument("Canonical command must use lowercase hex");
	};
	wire::Field field;
	field.tag = 1;
	field.required = true;
	field.value.reserve(hex.size() / 2);
	for (std::size_t index = 0; index < hex.size(); index += 2)
		field.value.push_back(static_cast<std::byte>((digit(hex[index]) << 4) | digit(hex[index + 1])));
	auto command = wire::DecodeSimulationCommand(field);
	if (wire::EncodeSimulationCommand(command).value != field.value)
		throw std::invalid_argument("Command is not in canonical form");
	return command;
}

SimulationCommand SimulationCommandFromJson(const Json& object)
{
	if (!object.is_object())
		throw std::invalid_argument("Simulation command must be an object");
	if (object.contains("encoding"))
		return Canonical(object);
	const auto kind = Text(object, "kind");
	if (kind == "take_control")
	{
		Keys(object, {"kind", "channel", "replace"});
		return TakeControl{Channel(object), Boolean(object, "replace", false)};
	}
	if (kind == "release_control")
	{
		Keys(object, {"kind", "channel"});
		return ReleaseControl{Channel(object)};
	}
	if (kind == "head_pose")
	{
		Keys(object, {"kind", "position", "orientation", "flags"});
		return SetHeadPose{Sample(object)};
	}
	if (kind == "controller_pose")
	{
		Keys(object, {"kind", "hand", "position", "orientation", "flags", "connected", "aim"});
		ControllerPose value;
		value.pose = Sample(object);
		value.connected = Boolean(object, "connected", true);
		if (const auto aim = object.find("aim"); aim != object.end() && !aim->is_null())
		{
			Keys(*aim, {"position", "orientation", "flags"});
			value.aim = Sample(*aim);
		}
		return SetControllerPose{Device(object, "hand", 1), std::move(value)};
	}
	if (kind == "controller_controls")
	{
		Keys(object, {"kind",
					  "hand",
					  "trigger",
					  "grip",
					  "grip_force",
					  "stick_x",
					  "stick_y",
					  "trackpad_x",
					  "trackpad_y",
					  "trackpad_force",
					  "primary",
					  "secondary",
					  "menu",
					  "back",
					  "thumb_click",
					  "trackpad_click",
					  "primary_touch",
					  "secondary_touch",
					  "trigger_touch",
					  "thumb_touch",
					  "trackpad_touch",
					  "thumbrest_touch",
					  "connected",
					  "trigger_click",
					  "physical_profile"});
		ControllerControls value;
		value.trigger = Axis(object, "trigger");
		value.grip = Axis(object, "grip");
		value.gripForce = Axis(object, "grip_force");
		value.stick = {Axis(object, "stick_x", -1), Axis(object, "stick_y", -1)};
		value.trackpad = {Axis(object, "trackpad_x", -1), Axis(object, "trackpad_y", -1)};
		value.trackpadForce = Axis(object, "trackpad_force");
		value.primary = Boolean(object, "primary", false);
		value.secondary = Boolean(object, "secondary", false);
		value.menu = Boolean(object, "menu", false);
		value.back = Boolean(object, "back", false);
		value.thumbClick = Boolean(object, "thumb_click", false);
		value.trackpadClick = Boolean(object, "trackpad_click", false);
		value.primaryTouch = Boolean(object, "primary_touch", false);
		value.secondaryTouch = Boolean(object, "secondary_touch", false);
		value.triggerTouch = Boolean(object, "trigger_touch", false);
		value.thumbTouch = Boolean(object, "thumb_touch", false);
		value.trackpadTouch = Boolean(object, "trackpad_touch", false);
		value.thumbrestTouch = Boolean(object, "thumbrest_touch", false);
		value.connected = Boolean(object, "connected", true);
		if (object.contains("trigger_click") && !object["trigger_click"].is_null())
			value.triggerClick = Boolean(object, "trigger_click", false);
		if (const auto profile = object.find("physical_profile"); profile != object.end())
		{
			if (!profile->is_string())
				throw std::invalid_argument("physical_profile must be a string");
			if (!profile->get<std::string>().empty())
				value.physicalProfile = Text(object, "physical_profile");
		}
		return SetControllerControls{Device(object, "hand", 1), std::move(value)};
	}
	if (kind == "body")
		return Body(object);
	if (kind == "tracker")
		return SetTracker{Device(object, "index", MaxTrackerDevices - 1), Tracker(object)};
	if (kind == "trackers")
	{
		Keys(object, {"kind", "devices"});
		const auto devices = object.find("devices");
		if (devices == object.end() || !devices->is_array() || devices->size() > MaxTrackerDevices)
			throw std::invalid_argument("Tracker inventory requires a bounded devices array");
		SetTrackers result;
		for (std::size_t index = 0; index < devices->size(); ++index)
		{
			const auto& entry = (*devices)[index];
			if (!entry.is_object() || (entry.contains("kind") && Text(entry, "kind") != "tracker") ||
				(entry.contains("index") && Integer(entry, "index", 0, MaxTrackerDevices - 1) != index))
				throw std::invalid_argument("Invalid tracker inventory entry or index");
			result.value.push_back(Tracker(entry));
		}
		return result;
	}
	if (kind == "gaze")
	{
		Keys(object, {"kind", "yaw", "pitch", "available", "tracked"});
		return SetGaze{{Float(object, "yaw", 0), Float(object, "pitch", 0), Boolean(object, "available", true),
						Boolean(object, "tracked", true)}};
	}
	if (kind == "proportions")
	{
		Keys(object, {"kind", "height", "shoulder_width", "hip_width"});
		return SetBodyProportions{
			{Float(object, "height", 1.7f), Float(object, "shoulder_width", 0.38f), Float(object, "hip_width", 0.24f)}};
	}
	if (kind == "controller_profile")
	{
		Keys(object, {"kind", "hand", "path"});
		return SetControllerProfile{Device(object, "hand", 1), Text(object, "path")};
	}
	if (kind == "eye_views")
	{
		Keys(object, {"kind", "enabled", "space_revision", "left", "right"});
		if (!Boolean(object, "enabled", true))
		{
			if (object.contains("space_revision") || object.contains("left") || object.contains("right"))
				throw std::invalid_argument("Cleared eye views cannot contain observations");
			return SetEyeViews{};
		}
		StereoEyeViews value;
		if (object.contains("space_revision"))
			value.spaceRevision = WideInteger(object, "space_revision", UINT64_MAX);
		for (std::size_t eye = 0; eye < 2; ++eye)
		{
			const auto entry = object.find(eye == 0 ? "left" : "right");
			if (entry == object.end())
				throw std::invalid_argument("Both eye observations are required");
			Keys(*entry, {"position", "orientation", "flags", "fov"});
			if (!entry->contains("fov"))
				throw std::invalid_argument("Each eye observation requires fov");
			value.poses[eye] = Sample(*entry);
			const auto fov = Vector<4>(*entry, "fov", {});
			value.fovs[eye] = {fov[0], fov[1], fov[2], fov[3]};
		}
		return SetEyeViews{value};
	}
	if (kind == "reference_spaces")
	{
		Keys(object, {"kind", "local_position", "local_orientation", "stage_position", "stage_orientation",
					  "bounds_available", "stage_bounds"});
		ReferenceSpaceState value;
		value.local = Pose(object, "local_position", "local_orientation");
		value.stage = Pose(object, "stage_position", "stage_orientation");
		const auto available = Boolean(object, "bounds_available", true);
		const auto bounds =
			Vector<2>(object, "stage_bounds", available ? std::array<float, 2>{2, 2} : std::array<float, 2>{0, 0});
		if (!available && (bounds[0] != 0 || bounds[1] != 0))
			throw std::invalid_argument("Unavailable stage bounds must have zero extent");
		value.stageBounds = available ? std::optional{XrExtent2Df{bounds[0], bounds[1]}} : std::nullopt;
		return SetReferenceSpaces{value};
	}
	if (kind == "views")
	{
		Keys(object, {"kind", "ipd", "inset_half_angle", "outer_fov", "outer_width", "outer_height", "inset_width",
					  "inset_height"});
		ViewSettings value;
		value.ipd = Float(object, "ipd", value.ipd);
		value.insetHalfAngle = Float(object, "inset_half_angle", value.insetHalfAngle);
		value.outerWidth = Integer(object, "outer_width", value.outerWidth, UINT32_MAX);
		value.outerHeight = Integer(object, "outer_height", value.outerHeight, UINT32_MAX);
		value.insetWidth = Integer(object, "inset_width", value.insetWidth, UINT32_MAX);
		value.insetHeight = Integer(object, "inset_height", value.insetHeight, UINT32_MAX);
		const auto fov = Vector<8>(object, "outer_fov", {-0.9f, 0.9f, 0.9f, -0.9f, -0.9f, 0.9f, 0.9f, -0.9f});
		for (std::size_t eye = 0; eye < 2; ++eye)
			value.outerFov[eye] = {fov[eye * 4], fov[eye * 4 + 1], fov[eye * 4 + 2], fov[eye * 4 + 3]};
		return SetViewSettings{value};
	}
	if (kind == "scenario")
	{
		Keys(object, {"kind", "scenario", "enabled"});
		static constexpr std::array names{
			"head_untracked",	"head_unavailable",	 "body_untracked",	   "body_unavailable",		"gaze_untracked",
			"gaze_unavailable", "left_disconnected", "right_disconnected", "trackers_disconnected", "unfocused",
			"hidden",			"recenter",			 "session_loss"};
		const auto it = object.find("scenario");
		if (it == object.end())
			throw std::invalid_argument("scenario is required");
		std::uint32_t scenario;
		if (it->is_string())
		{
			const auto name = it->get<std::string>();
			const auto found = std::find(names.begin(), names.end(), name);
			if (found == names.end())
				throw std::invalid_argument("Unknown scenario");
			scenario = static_cast<std::uint32_t>(found - names.begin());
		}
		else
			scenario = Integer(object, "scenario", UINT32_MAX, names.size() - 1);
		return SetScenario{static_cast<FailureScenario>(scenario), Boolean(object, "enabled", true)};
	}
	throw std::invalid_argument("Unsupported typed command; use its canonical simxr-command-v1-hex encoding");
}
} // namespace simxr::app::mcp
