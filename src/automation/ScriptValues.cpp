#include "automation/ScriptValues.h"
#include "Pch.h"

#include "control/SimulationControl.h"
#include "simulation/BodyObservation.h"

#include <lua.h>

namespace simxr::automation::script
{
namespace wire = control;

std::uint64_t Integer(lua_State* state, int index)
{
	if (lua_type(state, index) == LUA_TSTRING)
	{
		std::size_t length = 0;
		const char* text = lua_tolstring(state, index, &length);
		std::uint64_t value = 0;
		const auto parsed = std::from_chars(text, text + length, value);
		if (parsed.ec == std::errc{} && parsed.ptr == text + length && length && (length == 1 || text[0] != '0'))
			return value;
	}
	else if (lua_type(state, index) == LUA_TNUMBER)
	{
		const auto value = lua_tonumber(state, index);
		if (std::isfinite(value) && value >= 0 && value <= 9007199254740991.0 && std::floor(value) == value)
			return static_cast<std::uint64_t>(value);
	}
	throw std::invalid_argument("Expected an exact unsigned integer or canonical decimal string");
}

std::string String(lua_State* state, int index)
{
	if (lua_type(state, index) != LUA_TSTRING)
		throw std::invalid_argument("Expected a string");
	std::size_t length = 0;
	const auto* value = lua_tolstring(state, index, &length);
	if (length > 65536 || std::string_view{value, length}.find('\0') != std::string_view::npos)
		throw std::invalid_argument("Invalid bounded string");
	return {value, length};
}

static float FiniteFloat(double value)
{
	if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
		throw std::invalid_argument("Expected a finite float value");
	return static_cast<float>(value);
}

static float FloatField(lua_State* state, int table, const char* key, float fallback)
{
	lua_getfield(state, table, key);
	const auto type = lua_type(state, -1);
	const auto value = type == LUA_TNIL ? fallback : lua_tonumber(state, -1);
	lua_pop(state, 1);
	if (type != LUA_TNIL && type != LUA_TNUMBER)
		throw std::invalid_argument("Expected a finite number");
	return FiniteFloat(value);
}

static bool BoolField(lua_State* state, int table, const char* key, bool fallback)
{
	lua_getfield(state, table, key);
	const auto type = lua_type(state, -1);
	const bool value = type == LUA_TNIL ? fallback : lua_toboolean(state, -1) != 0;
	lua_pop(state, 1);
	if (type != LUA_TNIL && type != LUA_TBOOLEAN)
		throw std::invalid_argument("Expected a boolean");
	return value;
}

std::uint64_t IntegerField(lua_State* state, int table, const char* key, std::uint64_t fallback)
{
	lua_getfield(state, table, key);
	const auto value = lua_isnil(state, -1) ? fallback : Integer(state, -1);
	lua_pop(state, 1);
	return value;
}

std::uint32_t IndexField(lua_State* state, int table, const char* key, std::uint32_t fallback)
{
	const auto value = IntegerField(state, table, key, fallback);
	if (value > std::numeric_limits<std::uint32_t>::max())
		throw std::invalid_argument("Index exceeds uint32");
	return static_cast<std::uint32_t>(value);
}

std::string StringField(lua_State* state, int table, const char* key, std::string fallback)
{
	lua_getfield(state, table, key);
	auto value = lua_isnil(state, -1) ? std::move(fallback) : String(state, -1);
	lua_pop(state, 1);
	return value;
}

ScriptCaptureTarget CaptureTarget(lua_State* state, int index)
{
	if (!lua_istable(state, index))
		throw std::invalid_argument("Capture options require a table");
	const auto name = StringField(state, index, "target", "composed");
	ScriptCaptureTarget target;
	if (name == "composed")
		target.kind = wire::TargetKind::ComposedEyes;
	else if (name == "layer_view")
		target.kind = wire::TargetKind::LayerView;
	else if (name == "layer_depth")
		target.kind = wire::TargetKind::LayerDepth;
	else if (name == "layer_depth_linear")
		target.kind = wire::TargetKind::LayerDepthLinear;
	else if (name == "projection_layers")
		target.kind = wire::TargetKind::ProjectionLayers;
	else if (name == "quad_layers")
		target.kind = wire::TargetKind::QuadLayers;
	else
		throw std::invalid_argument("Unknown capture target");
	target.layer = IndexField(state, index, "layer", 0);
	target.view = IndexField(state, index, "view", 0);
	if ((target.kind == wire::TargetKind::ComposedEyes || target.kind == wire::TargetKind::ProjectionLayers ||
		 target.kind == wire::TargetKind::QuadLayers) &&
		(target.layer || target.view))
		throw std::invalid_argument("Composed capture cannot select a source layer/view");
	return target;
}

wire::TargetKind HistoryTarget(lua_State* state, int index)
{
	const auto target = lua_isnoneornil(state, index) ? ScriptCaptureTarget{} : CaptureTarget(state, index);
	if (target.kind != wire::TargetKind::ComposedEyes && target.kind != wire::TargetKind::QuadLayers)
		throw std::invalid_argument("History target must be composed or quad_layers");
	return target.kind;
}

std::optional<std::uint32_t> HistorySlice(lua_State* state, int index)
{
	if (lua_isnoneornil(state, index))
		return {};
	const auto slice = Integer(state, index);
	if (slice > 1)
		throw std::invalid_argument("Historical eye slice must be 0 or 1");
	return static_cast<std::uint32_t>(slice);
}

static std::vector<float> Vector(lua_State* state, int table, const char* key, std::initializer_list<float> fallback)
{
	lua_getfield(state, table, key);
	std::vector<float> values(fallback);
	if (!lua_isnil(state, -1))
	{
		if (!lua_istable(state, -1) || lua_objlen(state, -1) != static_cast<int>(values.size()))
			throw std::invalid_argument("Invalid vector length");
		for (std::size_t i = 0; i < values.size(); ++i)
		{
			lua_rawgeti(state, -1, static_cast<int>(i + 1));
			if (lua_type(state, -1) != LUA_TNUMBER)
				throw std::invalid_argument("Invalid vector component");
			values[i] = FiniteFloat(lua_tonumber(state, -1));
			lua_pop(state, 1);
		}
	}
	lua_pop(state, 1);
	return values;
}

static PoseSample Pose(lua_State* state, int table)
{
	const auto position = Vector(state, table, "position", {0, 0, 0});
	const auto orientation = Vector(state, table, "orientation", {0, 0, 0, 1});
	return {{{orientation[0], orientation[1], orientation[2], orientation[3]}, {position[0], position[1], position[2]}},
			IntegerField(state, table, "flags", TrackedPoseFlags)};
}

static constexpr std::array s_channelNames{"head",			 "left_pose",	"right_pose", "left_controls",
										   "right_controls", "body",		"gaze",		  "trackers",
										   "views",			 "proportions", "profiles",	  "session"};
static constexpr std::array s_scenarioNames{
	"head_untracked",	"head_unavailable",	 "body_untracked",	   "body_unavailable",		"gaze_untracked",
	"gaze_unavailable", "left_disconnected", "right_disconnected", "trackers_disconnected", "unfocused",
	"hidden",			"recenter",			 "session_loss"};

template<class Names>
static std::uint32_t Named(lua_State* state, int table, const char* key, const Names& names)
{
	lua_getfield(state, table, key);
	std::uint64_t value;
	if (lua_type(state, -1) == LUA_TSTRING)
	{
		const auto name = String(state, -1);
		const auto found = std::find(names.begin(), names.end(), name);
		if (found == names.end())
			throw std::invalid_argument("Unknown named command value");
		value = std::distance(names.begin(), found);
	}
	else
		value = Integer(state, -1);
	lua_pop(state, 1);
	if (value >= names.size())
		throw std::invalid_argument("Command enum is out of range");
	return static_cast<std::uint32_t>(value);
}

static ControllerControls ReadControls(lua_State* state, int table)
{
	ControllerControls controls;
	controls.trigger = FloatField(state, table, "trigger", 0);
	controls.grip = FloatField(state, table, "grip", 0);
	controls.gripForce = FloatField(state, table, "grip_force", 0);
	controls.stick.x = FloatField(state, table, "stick_x", 0);
	controls.stick.y = FloatField(state, table, "stick_y", 0);
	controls.trackpad.x = FloatField(state, table, "trackpad_x", 0);
	controls.trackpad.y = FloatField(state, table, "trackpad_y", 0);
	controls.trackpadForce = FloatField(state, table, "trackpad_force", 0);
	controls.primary = BoolField(state, table, "primary", false);
	controls.secondary = BoolField(state, table, "secondary", false);
	controls.menu = BoolField(state, table, "menu", false);
	controls.back = BoolField(state, table, "back", false);
	controls.thumbClick = BoolField(state, table, "thumb_click", false);
	controls.trackpadClick = BoolField(state, table, "trackpad_click", false);
	controls.primaryTouch = BoolField(state, table, "primary_touch", false);
	controls.secondaryTouch = BoolField(state, table, "secondary_touch", false);
	controls.triggerTouch = BoolField(state, table, "trigger_touch", false);
	lua_getfield(state, table, "trigger_click");
	if (!lua_isnil(state, -1))
		controls.triggerClick = BoolField(state, table, "trigger_click", false);
	lua_pop(state, 1);
	controls.thumbTouch = BoolField(state, table, "thumb_touch", false);
	controls.trackpadTouch = BoolField(state, table, "trackpad_touch", false);
	controls.thumbrestTouch = BoolField(state, table, "thumbrest_touch", false);
	controls.connected = BoolField(state, table, "connected", true);
	controls.physicalProfile = StringField(state, table, "physical_profile", "");
	return controls;
}

static BodyControls ReadBody(lua_State* state, int table)
{
	BodyControls body;
	body.active = BoolField(state, table, "active", true);
	body.confidence = FloatField(state, table, "confidence", 1);
	body.preset = static_cast<BodyPreset>(IndexField(state, table, "preset", 0));
	lua_getfield(state, table, "root_pose");
	if (!lua_isnil(state, -1))
	{
		if (!lua_istable(state, -1))
			throw std::invalid_argument("Invalid body root pose");
		body.rootPose = Pose(state, lua_absindex(state, -1)).pose;
	}
	lua_pop(state, 1);
	lua_getfield(state, table, "controls");
	if (!lua_isnil(state, -1))
	{
		if (!lua_istable(state, -1) || lua_objlen(state, -1) != 5)
			throw std::invalid_argument("Expected five body controls");
		for (int i = 0; i < 5; ++i)
		{
			lua_rawgeti(state, -1, i + 1);
			const auto child = lua_absindex(state, -1);
			body.controls[i].tracked = BoolField(state, child, "tracked", true);
			lua_getfield(state, child, "offset");
			const auto hasOffset = !lua_isnil(state, -1);
			lua_pop(state, 1);
			if (hasOffset)
			{
				const auto offset = Vector(state, child, "offset", {0, 0, 0});
				body.controls[i].offset = XrVector3f{offset[0], offset[1], offset[2]};
			}
			lua_pop(state, 1);
		}
	}
	lua_pop(state, 1);
	lua_getfield(state, table, "joints");
	if (!lua_isnil(state, -1))
	{
		if (!lua_istable(state, -1) || lua_objlen(state, -1) > static_cast<int>(BodyJointCount))
			throw std::invalid_argument("Invalid body joint overrides");
		for (int i = 1, count = lua_objlen(state, -1); i <= count; ++i)
		{
			lua_rawgeti(state, -1, i);
			const auto child = lua_absindex(state, -1);
			const auto joint = IndexField(state, child, "joint", std::numeric_limits<std::uint32_t>::max());
			if (joint >= BodyJointCount || body.joints[joint])
				throw std::invalid_argument("Invalid or duplicate body joint");
			BodyJointOverride value;
			value.tracked = BoolField(state, child, "tracked", true);
			lua_getfield(state, child, "position");
			const auto hasPosition = !lua_isnil(state, -1);
			lua_pop(state, 1);
			lua_getfield(state, child, "orientation");
			const auto hasOrientation = !lua_isnil(state, -1);
			lua_pop(state, 1);
			if (hasPosition || hasOrientation)
				value.pose = Pose(state, child).pose;
			body.joints[joint] = value;
			lua_pop(state, 1);
		}
	}
	lua_pop(state, 1);
	return body;
}

static bool HasField(lua_State* state, int table, const char* key)
{
	lua_getfield(state, table, key);
	const bool present = !lua_isnil(state, -1);
	lua_pop(state, 1);
	return present;
}

static RuntimeTime BodyTime(lua_State* state, int table, const char* key)
{
	const auto value = IntegerField(state, table, key, 0);
	if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
		throw std::invalid_argument("Observed body time exceeds signed nanoseconds");
	return {Duration{static_cast<std::int64_t>(value)}};
}

static int BodyArraySize(lua_State* state, int table)
{
	if (!lua_istable(state, table))
		throw std::invalid_argument("Observed body joints require an array");
	const int count = lua_objlen(state, table);
	if (count > static_cast<int>(BodyJointCount))
		throw std::invalid_argument("Observed body joints exceed capacity");
	int entries = 0;
	lua_pushnil(state);
	while (lua_next(state, table))
	{
		const double key = lua_tonumber(state, -2);
		if (lua_type(state, -2) != LUA_TNUMBER || !std::isfinite(key) || key < 1 || key > count ||
			std::floor(key) != key || !lua_istable(state, -1))
			throw std::invalid_argument("Observed body joints require contiguous table entries");
		++entries;
		lua_pop(state, 1);
	}
	if (entries != count)
		throw std::invalid_argument("Observed body joint array contains holes");
	return count;
}

static BodyObservation ReadObservation(lua_State* state, int table)
{
	if (!lua_istable(state, table) || !HasField(state, table, "joint_set") ||
		!HasField(state, table, "requested_time") || !HasField(state, table, "located"))
		throw std::invalid_argument("Observed body requires a joint set, requested time and located flag");
	BodyObservation body;
	const auto jointSet = IndexField(state, table, "joint_set", 0);
	if (jointSet > static_cast<std::uint32_t>(BodyJointSet::FullBodyBD))
		throw std::invalid_argument("Unknown observed body joint set");
	body.jointSet = static_cast<BodyJointSet>(jointSet);
	body.requestedTime = BodyTime(state, table, "requested_time");
	body.located = BoolField(state, table, "located", false);
	if (HasField(state, table, "returned_time"))
		body.returnedTime = BodyTime(state, table, "returned_time");
	if (HasField(state, table, "active"))
		body.active = BoolField(state, table, "active", false);
	if (HasField(state, table, "all_joint_poses_tracked"))
		body.allJointPosesTracked = BoolField(state, table, "all_joint_poses_tracked", false);
	if (HasField(state, table, "confidence"))
		body.confidence = FloatField(state, table, "confidence", 0);
	if (HasField(state, table, "skeleton_revision"))
		body.skeletonRevision = IndexField(state, table, "skeleton_revision", 0);
	for (const bool skeleton : {false, true})
	{
		lua_getfield(state, table, skeleton ? "skeleton" : "joints");
		if (skeleton && lua_isnil(state, -1))
		{
			lua_pop(state, 1);
			continue;
		}
		const auto array = lua_absindex(state, -1);
		const int count = BodyArraySize(state, array);
		if (skeleton)
			body.skeleton.emplace();
		for (int index = 1; index <= count; ++index)
		{
			lua_rawgeti(state, array, index);
			const auto child = lua_absindex(state, -1);
			const auto joint = IndexField(state, child, "joint", std::numeric_limits<std::uint32_t>::max());
			if (joint >= BodyJointCount || !HasField(state, child, "position") ||
				!HasField(state, child, "orientation"))
				throw std::invalid_argument("Observed body joint requires an identity and complete pose fields");
			const auto pose = Pose(state, child);
			if (skeleton)
			{
				if ((*body.skeleton)[joint])
					throw std::invalid_argument("Duplicate skeleton joint");
				SkeletonJoint value{static_cast<BodyJoint>(joint), {}, pose.pose};
				if (HasField(state, child, "parent"))
				{
					const auto parent = IndexField(state, child, "parent", 0);
					if (parent >= BodyJointCount)
						throw std::invalid_argument("Invalid observed skeleton parent");
					value.parent = static_cast<BodyJoint>(parent);
				}
				(*body.skeleton)[joint] = value;
			}
			else
			{
				if (body.joints[joint] || !HasField(state, child, "flags"))
					throw std::invalid_argument("Duplicate body joint or missing validity flags");
				body.joints[joint] = ObservedBodyJoint{pose.pose, pose.flags};
			}
			lua_pop(state, 1);
		}
		lua_pop(state, 1);
	}
	return NormalizeBodyObservation(std::move(body));
}

static TrackerDevice ReadTracker(lua_State* state, int table)
{
	TrackerDevice tracker{{IntegerField(state, table, "identity", 0)},
						  static_cast<TrackerRole>(IndexField(state, table, "role", 4)),
						  BoolField(state, table, "connected", true),
						  BoolField(state, table, "tracked", true)};
	tracker.poseFlags = IntegerField(state, table, "flags", TrackedPoseFlags);
	tracker.sourcePath = TrackerSourcePath{StringField(state, table, "source_path", {})};
	lua_getfield(state, table, "anchor");
	if (!lua_isnil(state, -1))
	{
		const auto anchor = Integer(state, -1);
		if (anchor >= BodyJointCount)
			throw std::invalid_argument("Tracker anchor exceeds the body joint range");
		tracker.anchor = static_cast<BodyJoint>(anchor);
	}
	lua_pop(state, 1);
	lua_getfield(state, table, "position");
	const auto hasPosition = !lua_isnil(state, -1);
	lua_pop(state, 1);
	lua_getfield(state, table, "orientation");
	const auto hasOrientation = !lua_isnil(state, -1);
	lua_pop(state, 1);
	if (hasPosition || hasOrientation)
		tracker.poseOverride = Pose(state, table).pose;
	lua_getfield(state, table, "joint_offset");
	if (!lua_isnil(state, -1))
		tracker.jointOffset = Pose(state, lua_gettop(state)).pose;
	lua_pop(state, 1);
	return tracker;
}

static ViewSettings ReadViews(lua_State* state, int table)
{
	ViewSettings settings;
	settings.ipd = FloatField(state, table, "ipd", settings.ipd);
	settings.insetHalfAngle = FloatField(state, table, "inset_half_angle", settings.insetHalfAngle);
	settings.outerWidth = IndexField(state, table, "outer_width", settings.outerWidth);
	settings.outerHeight = IndexField(state, table, "outer_height", settings.outerHeight);
	settings.insetWidth = IndexField(state, table, "inset_width", settings.insetWidth);
	settings.insetHeight = IndexField(state, table, "inset_height", settings.insetHeight);
	const auto fovAngles = Vector(state, table, "outer_fov", {-0.9f, 0.9f, 0.9f, -0.9f, -0.9f, 0.9f, 0.9f, -0.9f});
	settings.outerFov = {{{fovAngles[0], fovAngles[1], fovAngles[2], fovAngles[3]},
						  {fovAngles[4], fovAngles[5], fovAngles[6], fovAngles[7]}}};
	return settings;
}

SimulationCommand Command(lua_State* state, int index)
{
	if (lua_type(state, index) == LUA_TSTRING)
	{
		const auto text = String(state, index);
		if (text.size() % 2)
			throw std::invalid_argument("Invalid canonical command hex");
		wire::Field encoded;
		for (std::size_t i = 0; i < text.size(); i += 2)
		{
			unsigned value;
			const auto parsed = std::from_chars(text.data() + i, text.data() + i + 2, value, 16);
			if (parsed.ec != std::errc{} || parsed.ptr != text.data() + i + 2)
				throw std::invalid_argument("Invalid command hex");
			encoded.value.push_back(static_cast<std::byte>(value));
		}
		return wire::DecodeSimulationCommand(encoded);
	}
	if (!lua_istable(state, index))
		throw std::invalid_argument("Expected a command table");
	const auto table = lua_absindex(state, index);
	const auto kind = StringField(state, table, "kind");
	if (kind == "take_control")
		return TakeControl{static_cast<ControlChannel>(Named(state, table, "channel", s_channelNames)),
						   BoolField(state, table, "replace", false)};
	if (kind == "release_control")
		return ReleaseControl{static_cast<ControlChannel>(Named(state, table, "channel", s_channelNames))};
	if (kind == "head_pose")
	{
		const auto linear = Vector(state, table, "linear_velocity", {0, 0, 0});
		const auto angular = Vector(state, table, "angular_velocity", {0, 0, 0});
		return SetHeadPose{Pose(state, table), {linear[0], linear[1], linear[2]}, {angular[0], angular[1], angular[2]}};
	}
	if (kind == "controller_pose")
	{
		ControllerPose value{Pose(state, table), BoolField(state, table, "connected", true)};
		lua_getfield(state, table, "aim");
		if (!lua_isnil(state, -1))
		{
			if (!lua_istable(state, -1))
				throw std::invalid_argument("Expected an aim pose table");
			value.aim = Pose(state, lua_absindex(state, -1));
		}
		lua_pop(state, 1);
		return SetControllerPose{IndexField(state, table, "hand", 0), value};
	}
	if (kind == "controller_controls")
		return SetControllerControls{IndexField(state, table, "hand", 0), ReadControls(state, table)};
	if (kind == "gaze")
		return SetGaze{{FloatField(state, table, "yaw", 0), FloatField(state, table, "pitch", 0),
						BoolField(state, table, "available", true), BoolField(state, table, "tracked", true)}};
	if (kind == "body")
	{
		SetBodyControls result{ReadBody(state, table)};
		lua_getfield(state, table, "observation");
		if (!lua_isnil(state, -1))
			result.observation = ReadObservation(state, lua_absindex(state, -1));
		lua_pop(state, 1);
		if (HasField(state, table, "observation_origin"))
		{
			result.observationOrigin = BodyTime(state, table, "observation_origin");
			if (!result.observation || result.observationOrigin->value.count() <= 0)
				throw std::invalid_argument("Body replay timing requires an observation and positive anchor");
		}
		return result;
	}
	if (kind == "trackers")
	{
		lua_getfield(state, table, "devices");
		if (!lua_istable(state, -1) || lua_objlen(state, -1) > static_cast<int>(MaxTrackerDevices))
			throw std::invalid_argument("Invalid tracker collection");
		const int count = lua_objlen(state, -1);
		int entries = 0;
		lua_pushnil(state);
		while (lua_next(state, -2))
		{
			const double key = lua_type(state, -2) == LUA_TNUMBER ? lua_tonumber(state, -2) : 0;
			if (key < 1 || key > count || std::floor(key) != key)
				throw std::invalid_argument("Invalid tracker collection index");
			++entries;
			lua_pop(state, 1);
		}
		if (entries != count)
			throw std::invalid_argument("Sparse tracker collection");
		SetTrackers result;
		for (int i = 1; i <= count; ++i)
		{
			lua_rawgeti(state, -1, i);
			if (!lua_istable(state, -1))
				throw std::invalid_argument("Invalid tracker collection entry");
			result.value.push_back(ReadTracker(state, lua_absindex(state, -1)));
			lua_pop(state, 1);
		}
		lua_pop(state, 1);
		return result;
	}
	if (kind == "tracker")
		return SetTracker{IndexField(state, table, "index", 0), ReadTracker(state, table)};
	if (kind == "eye_views")
	{
		if (!BoolField(state, table, "enabled", true))
			return SetEyeViews{};
		StereoEyeViews views;
		views.spaceRevision = IntegerField(state, table, "space_revision", 0);
		for (std::size_t eye = 0; eye < 2; ++eye)
		{
			lua_getfield(state, table, eye == 0 ? "left" : "right");
			if (!lua_istable(state, -1))
				throw std::invalid_argument("Expected an eye observation table");
			const auto child = lua_absindex(state, -1);
			views.poses[eye] = Pose(state, child);
			const auto fov = Vector(state, child, "fov", {-.9f, .9f, .9f, -.9f});
			views.fovs[eye] = {fov[0], fov[1], fov[2], fov[3]};
			lua_pop(state, 1);
		}
		return SetEyeViews{views};
	}
	if (kind == "views")
		return SetViewSettings{ReadViews(state, table)};
	if (kind == "proportions")
		return SetBodyProportions{{FloatField(state, table, "height", 1.7f),
								   FloatField(state, table, "shoulder_width", 0.38f),
								   FloatField(state, table, "hip_width", 0.24f)}};
	if (kind == "controller_profile")
		return SetControllerProfile{IndexField(state, table, "hand", 0), StringField(state, table, "path")};
	if (kind == "scenario")
		return SetScenario{static_cast<FailureScenario>(Named(state, table, "scenario", s_scenarioNames)),
						   BoolField(state, table, "enabled", true)};
	if (kind == "reference_spaces")
	{
		const auto localRotation = Vector(state, table, "local_orientation", {0, 0, 0, 1});
		const auto localPosition = Vector(state, table, "local_position", {0, 0, 0});
		const auto stageRotation = Vector(state, table, "stage_orientation", {0, 0, 0, 1});
		const auto stagePosition = Vector(state, table, "stage_position", {0, 0, 0});
		const auto bounds = Vector(state, table, "stage_bounds", {2, 2});
		ReferenceSpaceState spaces;
		spaces.local = {{localRotation[0], localRotation[1], localRotation[2], localRotation[3]},
						{localPosition[0], localPosition[1], localPosition[2]}};
		spaces.stage = {{stageRotation[0], stageRotation[1], stageRotation[2], stageRotation[3]},
						{stagePosition[0], stagePosition[1], stagePosition[2]}};
		spaces.stageBounds = BoolField(state, table, "bounds_available", true)
								 ? std::optional{XrExtent2Df{bounds[0], bounds[1]}}
								 : std::nullopt;
		return SetReferenceSpaces{spaces};
	}
	throw std::invalid_argument("Unknown simulation command kind");
}

void PushInteger(lua_State* state, std::uint64_t value)
{
	const auto text = std::to_string(value);
	lua_pushlstring(state, text.data(), text.size());
}

void PushIntegerField(lua_State* state, const char* name, std::uint64_t value)
{
	PushInteger(state, value);
	lua_setfield(state, -2, name);
}

static void PushNode(lua_State* state, std::span<const std::byte> bytes, unsigned depth, bool named)
{
	if (depth > 32)
		throw std::runtime_error("Inspection nesting exceeds the rig limit");
	auto node = wire::DecodeFieldViews(bytes);
	if (node.error != wire::DecodeError::None || node.fields.size() < 3)
		throw std::runtime_error("Malformed inspection document");
	const auto* name = node.Find(1);
	const auto label = name ? name->AsText() : std::nullopt;
	if (!label || (named && label->empty()) || (!named && !label->empty()))
		throw std::runtime_error("Invalid inspection member name");
	if (named)
		lua_pushlstring(state, label->data(), label->size());
	const auto* kind = node.Find(2);
	const auto* data = node.Find(3);
	if (!kind || !kind->AsU32() || !data)
		throw std::runtime_error("Malformed inspection node");
	switch (static_cast<wire::InspectionType>(*kind->AsU32()))
	{
	case wire::InspectionType::Null:
		lua_pushnil(state);
		break;
	case wire::InspectionType::Boolean:
		if (!data->AsBool())
			throw std::runtime_error("Invalid inspection bool");
		lua_pushboolean(state, *data->AsBool());
		break;
	case wire::InspectionType::Unsigned:
	case wire::InspectionType::Revision:
		if (!data->AsU64())
			throw std::runtime_error("Invalid inspection integer");
		PushInteger(state, *data->AsU64());
		break;
	case wire::InspectionType::Signed:
	{
		if (!data->AsI64())
			throw std::runtime_error("Invalid inspection integer");
		const auto value = std::to_string(*data->AsI64());
		lua_pushlstring(state, value.empty() ? "" : value.data(), value.size());
		break;
	}
	case wire::InspectionType::Float:
		if (!data->AsF32())
			throw std::runtime_error("Invalid inspection float");
		lua_pushnumber(state, *data->AsF32());
		break;
	case wire::InspectionType::Text:
	{
		if (!data->AsText())
			throw std::runtime_error("Invalid inspection text");
		const auto value = *data->AsText();
		lua_pushlstring(state, value.empty() ? "" : value.data(), value.size());
		break;
	}
	case wire::InspectionType::Object:
	{
		lua_newtable(state);
		const auto children = wire::DecodeFieldViews(data->value);
		if (children.error != wire::DecodeError::None)
			throw std::runtime_error("Invalid inspection object");
		for (const auto& child : children.fields)
		{
			PushNode(state, child.value, depth + 1, true);
			lua_rawset(state, -3);
		}
		break;
	}
	case wire::InspectionType::Array:
	{
		lua_newtable(state);
		const auto pages = wire::DecodeFieldViews(data->value);
		int index = 1;
		if (pages.error != wire::DecodeError::None)
			throw std::runtime_error("Invalid inspection array");
		for (const auto& page : pages.fields)
		{
			const auto children = wire::DecodeFieldViews(page.value);
			if (children.error != wire::DecodeError::None)
				throw std::runtime_error("Invalid inspection array page");
			for (const auto& child : children.fields)
			{
				PushNode(state, child.value, depth + 1, false);
				lua_rawseti(state, -2, index++);
			}
		}
		break;
	}
	default:
		throw std::runtime_error("Unknown inspection value type");
	}
}

void PushInspection(lua_State* state, std::span<const std::byte> bytes, unsigned depth)
{
	PushNode(state, bytes, depth, false);
}
} // namespace simxr::automation::script
