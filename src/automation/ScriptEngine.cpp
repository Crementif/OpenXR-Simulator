#include "automation/ScriptEngine.h"
#include "automation/RigProgress.h"
#include "automation/RigRuntime.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <lua.h>
#include <luacode.h>
#include <lualib.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace simxr::automation
{
namespace
{
namespace wire = control;
namespace tag = control::tag;
std::uint64_t Integer(lua_State* state, int index)
{
	if (lua_type(state, index) == LUA_TSTRING)
	{
		size_t length = 0;
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
	size_t length = 0;
	const auto* value = lua_tolstring(state, index, &length);
	if (length > 65536 || std::string_view{value, length}.find('\0') != std::string_view::npos)
		throw std::invalid_argument("Invalid bounded string");
	return {value, length};
}
float FloatField(lua_State* state, int table, const char* key, float fallback)
{
	lua_getfield(state, table, key);
	const auto type = lua_type(state, -1);
	const auto value = type == LUA_TNIL ? fallback : static_cast<float>(lua_tonumber(state, -1));
	lua_pop(state, 1);
	if ((type != LUA_TNIL && type != LUA_TNUMBER) || !std::isfinite(value))
		throw std::invalid_argument("Expected a finite number");
	return value;
}
bool BoolField(lua_State* state, int table, const char* key, bool fallback)
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
std::string StringField(lua_State* state, int table, const char* key, std::string fallback = {})
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
		target.kind == wire::TargetKind::QuadLayers) && (target.layer || target.view))
		throw std::invalid_argument("Composed capture cannot select a source layer/view");
	return target;
}
std::vector<float> Vector(lua_State* state, int table, const char* key, std::initializer_list<float> fallback)
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
			values[i] = static_cast<float>(lua_tonumber(state, -1));
			lua_pop(state, 1);
			if (!std::isfinite(values[i]))
				throw std::invalid_argument("Non-finite vector component");
		}
	}
	lua_pop(state, 1);
	return values;
}
PoseSample Pose(lua_State* state, int table)
{
	const auto position = Vector(state, table, "position", {0, 0, 0});
	const auto orientation = Vector(state, table, "orientation", {0, 0, 0, 1});
	return {{{orientation[0], orientation[1], orientation[2], orientation[3]}, {position[0], position[1], position[2]}},
			IntegerField(state, table, "flags", TrackedPoseFlags)};
}
constexpr std::array ChannelNames{"head", "left_pose", "right_pose", "left_controls", "right_controls", "body",
								  "gaze", "trackers",  "views",		 "proportions",	  "profiles",		"session"};
constexpr std::array ScenarioNames{
	"head_untracked",	"head_unavailable",	 "body_untracked",	   "body_unavailable",		"gaze_untracked",
	"gaze_unavailable", "left_disconnected", "right_disconnected", "trackers_disconnected", "unfocused",
	"hidden",			"recenter",			 "session_loss"};
template <class Names> std::uint32_t Named(lua_State* state, int table, const char* key, const Names& names)
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
ControllerControls ReadControls(lua_State* state, int table)
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
	controls.thumbTouch = BoolField(state, table, "thumb_touch", false);
	controls.trackpadTouch = BoolField(state, table, "trackpad_touch", false);
	controls.thumbrestTouch = BoolField(state, table, "thumbrest_touch", false);
	controls.connected = BoolField(state, table, "connected", true);
	return controls;
}
BodyControls ReadBody(lua_State* state, int table)
{
	BodyControls body;
	body.active = BoolField(state, table, "active", true);
	body.confidence = FloatField(state, table, "confidence", 1);
	body.preset = static_cast<BodyPreset>(IndexField(state, table, "preset", 0));
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
TrackerDevice ReadTracker(lua_State* state, int table)
{
	TrackerDevice tracker{{IntegerField(state, table, "identity", 0)},
						  static_cast<TrackerRole>(IndexField(state, table, "role", 4)),
						  BoolField(state, table, "connected", true),
						  BoolField(state, table, "tracked", true)};
	lua_getfield(state, table, "anchor");
	if (!lua_isnil(state, -1))
		tracker.anchor = static_cast<BodyJoint>(Integer(state, -1));
	lua_pop(state, 1);
	lua_getfield(state, table, "position");
	const auto hasPosition = !lua_isnil(state, -1);
	lua_pop(state, 1);
	lua_getfield(state, table, "orientation");
	const auto hasOrientation = !lua_isnil(state, -1);
	lua_pop(state, 1);
	if (hasPosition || hasOrientation)
		tracker.poseOverride = Pose(state, table).pose;
	return tracker;
}
ViewSettings ReadViews(lua_State* state, int table)
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
		return TakeControl{static_cast<ControlChannel>(Named(state, table, "channel", ChannelNames)),
						   BoolField(state, table, "replace", false)};
	if (kind == "release_control")
		return ReleaseControl{static_cast<ControlChannel>(Named(state, table, "channel", ChannelNames))};
	if (kind == "head_pose")
		return SetHeadPose{Pose(state, table)};
	if (kind == "controller_pose")
		return SetControllerPose{IndexField(state, table, "hand", 0),
								 {Pose(state, table), BoolField(state, table, "connected", true)}};
	if (kind == "controller_controls")
		return SetControllerControls{IndexField(state, table, "hand", 0), ReadControls(state, table)};
	if (kind == "gaze")
		return SetGaze{{FloatField(state, table, "yaw", 0), FloatField(state, table, "pitch", 0),
						BoolField(state, table, "available", true), BoolField(state, table, "tracked", true)}};
	if (kind == "body")
		return SetBodyControls{ReadBody(state, table)};
	if (kind == "tracker")
		return SetTracker{IndexField(state, table, "index", 0), ReadTracker(state, table)};
	if (kind == "views")
		return SetViewSettings{ReadViews(state, table)};
	if (kind == "proportions")
		return SetBodyProportions{{FloatField(state, table, "height", 1.7f),
								   FloatField(state, table, "shoulder_width", 0.38f),
								   FloatField(state, table, "hip_width", 0.24f)}};
	if (kind == "controller_profile")
		return SetControllerProfile{IndexField(state, table, "hand", 0), StringField(state, table, "path")};
	if (kind == "scenario")
		return SetScenario{static_cast<FailureScenario>(Named(state, table, "scenario", ScenarioNames)),
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
			? std::optional{XrExtent2Df{bounds[0], bounds[1]}} : std::nullopt;
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
void PushInspection(lua_State* state, std::span<const std::byte> bytes, unsigned depth = 0)
{
	if (depth > 32)
		throw std::runtime_error("Inspection nesting exceeds the rig limit");
	auto fields = wire::DecodeFields(bytes);
	if (fields.error != wire::DecodeError::None || fields.fields.size() < 3)
		throw std::runtime_error("Malformed inspection document");
	wire::Message node;
	node.fields = std::move(fields.fields);
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
		const auto children = wire::DecodeFields(data->value);
		if (children.error != wire::DecodeError::None)
			throw std::runtime_error("Invalid inspection object");
		for (const auto& child : children.fields)
		{
			wire::Message entry;
			const auto decoded = wire::DecodeFields(child.value);
			entry.fields = decoded.fields;
			const auto* nameField = entry.Find(1);
			if (!nameField || !nameField->AsText())
				throw std::runtime_error("Invalid inspection member name");
			const std::string name{*nameField->AsText()};
			PushInspection(state, child.value, depth + 1);
			lua_setfield(state, -2, name.c_str());
		}
		break;
	}
	case wire::InspectionType::Array:
	{
		lua_newtable(state);
		const auto pages = wire::DecodeFields(data->value);
		int index = 1;
		if (pages.error != wire::DecodeError::None)
			throw std::runtime_error("Invalid inspection array");
		for (const auto& page : pages.fields)
		{
			const auto children = wire::DecodeFields(page.value);
			if (children.error != wire::DecodeError::None)
				throw std::runtime_error("Invalid inspection array page");
			for (const auto& child : children.fields)
			{
				PushInspection(state, child.value, depth + 1);
				lua_rawseti(state, -2, index++);
			}
		}
		break;
	}
	}
}
} // namespace

class ScriptEngine::Impl
{
	RigRuntime& m_runtime;
	RigProgress& m_progress;
	const RigOptions& m_options;
	std::size_t m_allocated = 0;
	std::string m_bindingError;

  public:
	Impl(RigRuntime& runtime, RigProgress& progress, const RigOptions& options)
		: m_runtime(runtime), m_progress(progress), m_options(options)
	{
	}

  private:
	static Impl& Self(lua_State* state)
	{
		return *static_cast<Impl*>(lua_callbacks(state)->userdata);
	}
	static void* Allocate(void* context, void* pointer, std::size_t oldSize, std::size_t newSize)
	{
		auto& engine = *static_cast<Impl*>(context);
		if (!pointer)
			oldSize = 0;
		if (!newSize)
		{
			std::free(pointer);
			engine.m_allocated -= oldSize;
			return nullptr;
		}
		if (newSize > oldSize && newSize - oldSize > engine.m_options.memoryLimit - engine.m_allocated)
			return nullptr;
		auto* allocation = std::realloc(pointer, newSize);
		if (allocation)
			engine.m_allocated = engine.m_allocated - oldSize + newSize;
		return allocation;
	}
	static void Interrupt(lua_State* state, int gc)
	{
		if (gc >= 0)
			return;
		auto& engine = Self(state);
		try
		{
			engine.m_progress.Check();
			return;
		}
		catch (const std::exception& error)
		{
			engine.m_bindingError = error.what();
		}
		luaL_error(state, "%s", engine.m_bindingError.c_str());
	}
	template <int (Impl::*Handler)(lua_State*)> static int Binding(lua_State* state)
	{
		auto& engine = Self(state);
		try
		{
			engine.m_progress.Check(true);
			return (engine.*Handler)(state);
		}
		catch (const std::exception& error)
		{
			engine.m_bindingError = error.what();
		}
		catch (...)
		{
			engine.m_bindingError = "Unknown rig binding failure";
		}
		luaL_error(state, "%s", engine.m_bindingError.c_str());
	}
	int FrameAdd(lua_State* state)
	{
		const auto frame = Integer(state, 1);
		const auto offset = Integer(state, 2);
		if (offset > std::numeric_limits<std::uint64_t>::max() - frame)
			throw std::invalid_argument("Frame arithmetic exceeds uint64");
		PushInteger(state, frame + offset);
		return 1;
	}
	int FrameBefore(lua_State* state)
	{
		lua_pushboolean(state, Integer(state, 1) < Integer(state, 2));
		return 1;
	}
	int Schedule(lua_State* state)
	{
		if (!lua_istable(state, 1))
			throw std::invalid_argument("schedule requires a transaction table");
		CommandTarget target;
		const auto policyName = StringField(state, 1, "late_policy", "reject");
		CommandLatePolicy policy;
		if (policyName == "reject")
			policy = CommandLatePolicy::Reject;
		else if (policyName == "reschedule_next")
			policy = CommandLatePolicy::RescheduleNext;
		else
			throw std::invalid_argument("Invalid late policy");
		lua_getfield(state, 1, "frame");
		if (!lua_isnil(state, -1))
			target.frame = FrameId{Integer(state, -1)};
		lua_pop(state, 1);
		lua_getfield(state, 1, "time_ns");
		if (!lua_isnil(state, -1))
		{
			const auto time = Integer(state, -1);
			if (time > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
				throw std::invalid_argument("Simulation time exceeds int64");
			target.time = SimulationTime{Duration{time}};
		}
		lua_pop(state, 1);
		if (target.frame.has_value() == target.time.has_value())
			throw std::invalid_argument("Select frame or time_ns");
		lua_getfield(state, 1, "commands");
		if (!lua_istable(state, -1) || lua_objlen(state, -1) < 1 || lua_objlen(state, -1) > 64)
			throw std::invalid_argument("Expected 1 through 64 commands");
		std::vector<SimulationCommand> commands;
		for (int index = 1, count = lua_objlen(state, -1); index <= count; ++index)
		{
			lua_rawgeti(state, -1, index);
			commands.push_back(Command(state, -1));
			lua_pop(state, 1);
		}
		lua_pop(state, 1);
		PushInteger(state, m_runtime.Schedule(target, policy, std::move(commands)));
		return 1;
	}
	int AwaitTransaction(lua_State* state)
	{
		PushInspection(state, m_runtime.AwaitTransaction(Integer(state, 1)));
		return 1;
	}
	int Pause(lua_State*)
	{
		m_runtime.PauseApplication();
		return 0;
	}
	int Resume(lua_State*)
	{
		m_runtime.ResumeApplication();
		return 0;
	}
	int Step(lua_State* state)
	{
		ScriptCaptureTarget target;
		if (lua_istable(state, 1))
			target = CaptureTarget(state, 1);
		else if (!lua_isnoneornil(state, 1))
		{
			const auto layerIndex = Integer(state, 1);
			const auto viewIndex = lua_isnoneornil(state, 2) ? 0 : Integer(state, 2);
			if (layerIndex > UINT32_MAX || viewIndex > UINT32_MAX)
				throw std::invalid_argument("Capture index exceeds uint32");
			target = {wire::TargetKind::LayerView, static_cast<std::uint32_t>(layerIndex),
				static_cast<std::uint32_t>(viewIndex)};
		}
		const auto step = m_runtime.StepApplication(target);
		if (step.capture)
			PushInteger(state, *step.capture);
		else
			lua_pushnil(state);
		lua_pushlstring(state, step.progress.data(), step.progress.size());
		return 2;
	}
	int StartInputRecording(lua_State* state)
	{
		PushInspection(state, m_runtime.StartInputRecording());
		return 1;
	}
	int StopInputRecording(lua_State* state)
	{
		PushInspection(state, m_runtime.StopInputRecording());
		return 1;
	}
	int ReadInputRecording(lua_State* state)
	{
		const auto offset = lua_isnoneornil(state, 1) ? 0 : Integer(state, 1);
		const auto count = lua_isnoneornil(state, 2) ? 16 : Integer(state, 2);
		PushInspection(state, m_runtime.ReadInputRecording(offset, count));
		return 1;
	}
	int StartImageRecording(lua_State* state)
	{
		ScriptCaptureTarget target;
		std::uint32_t capacity = 2;
		std::uint64_t frames = 0;
		if (!lua_isnoneornil(state, 1))
		{
			target = CaptureTarget(state, 1);
			capacity = IndexField(state, 1, "capacity", 2);
			frames = IntegerField(state, 1, "frames", 0);
		}
		m_runtime.StartImageRecording(target, capacity, frames);
		return 0;
	}
	int StopImageRecording(lua_State*)
	{
		m_runtime.StopImageRecording();
		return 0;
	}
	int ImageRecording(lua_State* state)
	{
		const auto recording = m_runtime.ImageRecording();
		lua_newtable(state);
		lua_pushlstring(state, recording.phase.data(), recording.phase.size());
		lua_setfield(state, -2, "phase");
		lua_pushboolean(state, recording.available);
		lua_setfield(state, -2, "available");
		PushIntegerField(state, "capacity", recording.capacity);
		PushIntegerField(state, "captured", recording.captured);
		PushIntegerField(state, "requested", recording.requested);
		if (recording.missedFrame)
			PushIntegerField(state, "missed_frame", *recording.missedFrame);
		lua_newtable(state);
		int index = 1;
		for (const auto capture : recording.captures)
		{
			PushInteger(state, capture);
			lua_rawseti(state, -2, index++);
		}
		lua_setfield(state, -2, "captures");
		return 1;
	}
	int Capture(lua_State* state)
	{
		const auto capture = m_runtime.Capture(Integer(state, 1));
		lua_newtable(state);
		PushIntegerField(state, "capture_id", capture.identity);
		lua_pushlstring(state, capture.phase.data(), capture.phase.size());
		lua_setfield(state, -2, "phase");
		lua_pushboolean(state, capture.imageAvailable);
		lua_setfield(state, -2, "image_available");
		if (capture.frame)
			PushIntegerField(state, "frame_id", *capture.frame);
		return 1;
	}
	int Inspect(lua_State* state)
	{
		const auto inspection = m_runtime.Inspect(Integer(state, 1));
		PushInspection(state, inspection.document);
		PushInteger(state, inspection.identity);
		return 2;
	}
	int Revision(lua_State* state)
	{
		PushInspection(state, m_runtime.Revision(Integer(state, 1)));
		return 1;
	}
	int ReleaseInspection(lua_State* state)
	{
		m_runtime.ReleaseInspection(Integer(state, 1));
		return 0;
	}
	int StepDisplay(lua_State* state)
	{
		const auto step = m_runtime.StepDisplay();
		if (step.capture)
			PushInteger(state, *step.capture);
		else
			lua_pushnil(state);
		lua_pushlstring(state, step.progress.data(), step.progress.size());
		if (step.frame)
			PushInteger(state, step.frame->value);
		else
			lua_pushnil(state);
		return 3;
	}
	int State(lua_State* state)
	{
		PushInspection(state, m_runtime.State());
		return 1;
	}
	int ReleaseInputs(lua_State*)
	{
		m_runtime.ReleaseInputs();
		return 0;
	}
	int Export(lua_State* state)
	{
		std::optional<ScriptCaptureTarget> target;
		std::optional<std::uint32_t> arraySlice;
		if (!lua_isnoneornil(state, 3))
		{
			if (!lua_istable(state, 3))
				throw std::invalid_argument("Export options require a table");
			lua_getfield(state, 3, "target");
			const bool selectTarget = !lua_isnil(state, -1);
			lua_pop(state, 1);
			if (selectTarget)
				target = CaptureTarget(state, 3);
			lua_getfield(state, 3, "array_slice");
			const bool selectSlice = !lua_isnil(state, -1);
			lua_pop(state, 1);
			if (selectSlice)
				arraySlice = IndexField(state, 3, "array_slice", 0);
		}
		m_runtime.Export(Integer(state, 1), String(state, 2), target, arraySlice);
		return 0;
	}
	int ReleaseCapture(lua_State* state)
	{
		m_runtime.ReleaseCapture(Integer(state, 1));
		return 0;
	}
	int Checkpoint(lua_State* state)
	{
		RigCheckpoint checkpoint;
		checkpoint.name = String(state, 1);
		checkpoint.passed = lua_type(state, 2) == LUA_TBOOLEAN && lua_toboolean(state, 2);
		checkpoint.detail = lua_isnoneornil(state, 3) ? std::string{} : String(state, 3);
		m_progress.Checkpoint(std::move(checkpoint));
		return 0;
	}
	int Print(lua_State* state)
	{
		std::string line;
		for (int index = 1, count = lua_gettop(state); index <= count; ++index)
		{
			if (index != 1)
				line += '\t';
			if (lua_isstring(state, index))
			{
				std::size_t length;
				const auto* value = lua_tolstring(state, index, &length);
				line.append(value, length);
			}
			else
				line += lua_typename(state, lua_type(state, index));
			if (line.size() > 4096)
				throw std::length_error("Rig output line exceeds 4096 bytes");
		}
		m_progress.Print(std::move(line));
		return 0;
	}
	int GameCommand(lua_State* state)
	{
		const auto kind = Integer(state, 1);
		if (kind < 1 || kind > 3)
			throw std::invalid_argument("Game command kind must be reset=1, step=2 or custom=3");
		const auto payload = lua_isnoneornil(state, 2) ? std::string{} : String(state, 2);
		const auto command = m_runtime.GameCommand(static_cast<std::uint32_t>(kind), payload);
		PushInteger(state, command.operation);
		PushInteger(state, command.game);
		return 2;
	}
	int AwaitGame(lua_State* state)
	{
		PushInspection(state, m_runtime.AwaitGame(Integer(state, 1), Integer(state, 2)));
		return 1;
	}
	int GameState(lua_State* state)
	{
		PushInspection(state, m_runtime.GameState());
		return 1;
	}

  public:
	void Execute()
	{
		std::unique_ptr<lua_State, decltype(&lua_close)> vm{lua_newstate(Allocate, this), lua_close};
		auto* state = vm.get();
		if (!state)
			throw std::bad_alloc();
		lua_callbacks(state)->userdata = this;
		lua_callbacks(state)->interrupt = Interrupt;
		luaL_openlibs(state);
		lua_pushcfunction(state, Binding<&Impl::Print>, "print");
		lua_setglobal(state, "print");
		lua_newtable(state);
		const auto bind = [&](const char* name, lua_CFunction function)
		{
			lua_pushcfunction(state, function, name);
			lua_setfield(state, -2, name);
		};
		bind("frame_add", Binding<&Impl::FrameAdd>);
		bind("frame_before", Binding<&Impl::FrameBefore>);
		bind("step_display", Binding<&Impl::StepDisplay>);
		bind("schedule", Binding<&Impl::Schedule>);
		bind("await_transaction", Binding<&Impl::AwaitTransaction>);
		bind("pause", Binding<&Impl::Pause>);
		bind("resume", Binding<&Impl::Resume>);
		bind("step", Binding<&Impl::Step>);
		bind("start_input_recording", Binding<&Impl::StartInputRecording>);
		bind("stop_input_recording", Binding<&Impl::StopInputRecording>);
		bind("read_input_recording", Binding<&Impl::ReadInputRecording>);
		bind("start_image_recording", Binding<&Impl::StartImageRecording>);
		bind("stop_image_recording", Binding<&Impl::StopImageRecording>);
		bind("image_recording", Binding<&Impl::ImageRecording>);
		bind("capture", Binding<&Impl::Capture>);
		bind("inspect", Binding<&Impl::Inspect>);
		bind("revision", Binding<&Impl::Revision>);
		bind("release_inspection", Binding<&Impl::ReleaseInspection>);
		bind("checkpoint", Binding<&Impl::Checkpoint>);
		bind("state", Binding<&Impl::State>);
		bind("release_inputs", Binding<&Impl::ReleaseInputs>);
		bind("export", Binding<&Impl::Export>);
		bind("release_capture", Binding<&Impl::ReleaseCapture>);
		bind("game_command", Binding<&Impl::GameCommand>);
		bind("await_game", Binding<&Impl::AwaitGame>);
		bind("game_state", Binding<&Impl::GameState>);
		PushInteger(state, m_options.seed);
		lua_setfield(state, -2, "seed");
		lua_setglobal(state, "simxr");
		lua_getglobal(state, "math");
		lua_getfield(state, -1, "randomseed");
		lua_pushnumber(state, static_cast<double>(m_options.seed & 0xffffffffULL));
		if (lua_pcall(state, 1, 0, 0) != LUA_OK)
			throw std::runtime_error("Cannot seed rig random generator");
		lua_pop(state, 1);
		luaL_sandbox(state);
		std::size_t bytecodeSize = 0;
		std::unique_ptr<char, decltype(&std::free)> bytecode{
			luau_compile(m_options.source.data(), m_options.source.size(), nullptr, &bytecodeSize), std::free};
		if (!bytecode)
			throw std::bad_alloc();
		const auto loaded = luau_load(state, m_options.name.c_str(), bytecode.get(), bytecodeSize, 0);
		bytecode.reset();
		if (loaded != LUA_OK || lua_pcall(state, 0, 0, 0) != LUA_OK)
			throw std::runtime_error(lua_tostring(state, -1) ? lua_tostring(state, -1) : "Luau execution failed");
		m_progress.Check();
	}
};

ScriptEngine::ScriptEngine(RigRuntime& runtime, RigProgress& progress, const RigOptions& options)
	: m_impl(std::make_unique<Impl>(runtime, progress, options))
{
}
ScriptEngine::~ScriptEngine() = default;
void ScriptEngine::Execute()
{
	m_impl->Execute();
}
} // namespace simxr::automation
