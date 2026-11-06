#include "app/mcp/McpPch.h"
#include "app/mcp/McpControls.h"
#include "app/mcp/McpSimulation.h"
#include "simulation/ControllerPoses.h"
#include "simulation/HeadsetProfiles.h"
#include "math/Math.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;

bool ControlsHandles(const std::string& name)
{
	static const std::set<std::string> names{"set_head_pose",			"set_controller_pose", "point_controller",
											 "set_controller_controls", "set_views",		   "set_headset_profile",
											 "get_headset_profiles"};
	return names.contains(name);
}

Json ControlsDefinitions()
{
	const Json text = {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}};
	const Json identifier = {
		{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^[1-9][0-9]{0,19}$"}},
							   {{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991ULL}}})}};
	const Json number = {{"type", "number"}};
	const Json boolean = {{"type", "boolean"}};
	const Json hand = {
		{"type", "integer"}, {"enum", Json::array({0, 1})}, {"description", "0 left, 1 right; required."}};
	const auto vector = [&](unsigned size) {
		return Json{{"type", "array"}, {"minItems", size}, {"maxItems", size}, {"items", number}};
	};
	const Json common = {
		{"connection", text}, {"session", identifier}, {"generation", identifier}, {"transaction_id", identifier}};
	const auto tool = [](const char* name, const char* description, Json properties, Json required = Json::array()) {
		required.push_back("connection");
		required.push_back("transaction_id");
		return Json{{"name", name},
					{"description",
					 std::string(description) +
						 " Applies at the next frame with this connection's input ownership. Poll simulation_status "
						 "for application; ownership or baseline conflicts reject the entire edit."},
					{"inputSchema",
					 {{"type", "object"},
					  {"properties", std::move(properties)},
					  {"required", std::move(required)},
					  {"additionalProperties", false}}}};
	};
	Json tools = Json::array();
	Json pose = {{"position", vector(3)},
				 {"orientation", vector(4)},
				 {"flags", {{"type", "integer"}, {"minimum", 0}, {"maximum", 15}}}};
	auto head = common;
	head.update(pose);
	tools.push_back(tool("set_head_pose",
						 "Replace head pose in stage space: position in metres, orientation quaternion [x,y,z,w]. "
						 "Omitted components use zero position, identity orientation and flags 15.",
						 head));
	auto controller = head;
	controller["hand"] = hand;
	controller["connected"] = boolean;
	controller["aim"] = {{"type", {"object", "null"}}, {"properties", pose}, {"additionalProperties", false}};
	tools.push_back(tool("set_controller_pose",
						 "Replace controller grip pose in stage space using metres and quaternion [x,y,z,w], with "
						 "optional independent aim pose. Defaults match head pose; connected defaults true.",
						 controller, {"hand"}));
	auto point = common;
	point["hand"] = hand;
	for (const auto* key : {"x", "y"})
		point[key] = {{"type", "number"}, {"minimum", 0}, {"maximum", 1}};
	point["eye"] = {{"type", "integer"}, {"enum", Json::array({0, 1})}, {"description", "0 left (default), 1 right."}};
	point["distance"] = {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1000}};
	tools.push_back(tool("point_controller",
						 "Turn a controller in place so its aim ray passes through a point of an eye image: x and y "
						 "run 0..1 from the image's top left, as in capture_screenshot. The point lies distance "
						 "metres (default 3) along that eye's view ray; set it near the target's depth, since the "
						 "hand sits apart from the eye. Sets the grip and an explicit aim pose.",
						 point, {"hand", "x", "y"}));
	auto controls = common;
	controls["hand"] = hand;
	for (const auto* key : {"trigger", "grip", "grip_force", "trackpad_force"})
		controls[key] = {{"type", "number"}, {"minimum", 0}, {"maximum", 1}};
	for (const auto* key : {"stick_x", "stick_y", "trackpad_x", "trackpad_y"})
		controls[key] = {{"type", "number"}, {"minimum", -1}, {"maximum", 1}};
	for (const auto* key :
		 {"primary", "secondary", "menu", "back", "thumb_click", "trackpad_click", "primary_touch", "secondary_touch",
		  "trigger_touch", "thumb_touch", "trackpad_touch", "thumbrest_touch", "connected"})
		controls[key] = boolean;
	controls["trigger_click"] = {{"type", {"boolean", "null"}}};
	controls["physical_profile"] = {{"type", "string"}, {"maxLength", 512}};
	tools.push_back(tool("set_controller_controls",
						 "Replace held controller inputs. Omitted axes/buttons reset to zero/false; connected defaults "
						 "true and trigger_click defaults to the runtime threshold. Buttons require JSON booleans.",
						 controls, {"hand"}));
	auto views = common;
	views["ipd"] = number;
	views["outer_fov"] = vector(8);
	views["inset_half_angle"] = number;
	views["refresh_rate"] = {{"type", "number"}, {"minimum", 0}, {"maximum", MaximumRefreshRate}};
	for (const auto* key : {"outer_width", "outer_height", "inset_width", "inset_height"})
		views[key] = {{"type", "integer"}, {"minimum", 1}, {"maximum", UINT32_MAX}};
	tools.push_back(
		tool("set_views",
			 "Update view settings, preserving omitted fields. IPD uses metres; FOV angles use radians. outer_fov "
			 "contains left/right/up/down for the left eye, then the right eye. Dimensions use pixels. "
			 "refresh_rate sets the display rate in hertz; 0 returns to the app's request or the launch rate.",
			 views));
	auto profile = common;
	Json names = Json::array();
	for (const auto& spec : HeadsetProfiles())
		names.push_back(spec.id);
	profile["name"] = {{"type", "string"}, {"enum", std::move(names)}};
	tools.push_back(tool("set_headset_profile",
						 "Apply named optics, preserving render dimensions. The generic profile restores 0.064 metre "
						 "IPD and symmetric pi/4 radian half angles.",
						 profile, {"name"}));
	tools.push_back(
		{{"name", "get_headset_profiles"},
		 {"description", "List nominal headset optics in native units: ipd in metres, outer_fov in radians, "
						 "recommended dimensions in pixels. No application is required."},
		 {"inputSchema", {{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}}}});
	return tools;
}

nlohmann::json HeadsetProfilesDocument()
{
	Json profiles = Json::array();
	for (const auto& spec : HeadsetProfiles())
	{
		const auto views = ProfileViews(spec);
		Json fovs = Json::array();
		for (const auto& eye : views.outerFov)
			for (const auto angle : {eye.angleLeft, eye.angleRight, eye.angleUp, eye.angleDown})
				fovs.push_back(angle);
		profiles.push_back({{"name", spec.id},
							{"ipd", views.ipd},
							{"outer_fov", std::move(fovs)},
							{"recommended_width", views.outerWidth},
							{"recommended_height", views.outerHeight}});
	}
	return {{"profiles", std::move(profiles)}};
}

static XrVector3f JsonVector(const Json& value)
{
	return {value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>()};
}

static XrPosef JsonPose(const Json& value)
{
	const auto& orientation = value.at("orientation");
	return {{orientation.at(0).get<float>(), orientation.at(1).get<float>(), orientation.at(2).get<float>(),
			 orientation.at(3).get<float>()},
			JsonVector(value.at("position"))};
}

static Json PoseJson(XrPosef pose, std::uint32_t flags)
{
	return {
		{"position", Json::array({pose.position.x, pose.position.y, pose.position.z})},
		{"orientation", Json::array({pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w})},
		{"flags", flags}};
}

static Json PointCommand(Json command, const Json& configuration)
{
	const auto number = [&](const char* key, double fallback) {
		if (!command.contains(key))
			return fallback;
		if (!command.at(key).is_number())
			throw std::invalid_argument(std::string(key) + " must be a number");
		return command.at(key).get<double>();
	};
	const auto x = number("x", -1), y = number("y", -1), distance = number("distance", DefaultPointDistance);
	const auto eye = number("eye", 0);
	if (!(x >= 0 && x <= 1 && y >= 0 && y <= 1))
		throw std::invalid_argument("x and y must lie in 0..1");
	if (!(distance > 0 && distance <= 1000))
		throw std::invalid_argument("distance must be above 0 and at most 1000 metres");
	if (eye != 0 && eye != 1)
		throw std::invalid_argument("eye must be 0 (left) or 1 (right)");
	const auto index = static_cast<std::size_t>(eye);
	XrPosef view;
	XrFovf fov;
	if (const auto& eyes = configuration.at("eye_views"); !eyes.is_null())
	{
		const auto& entry = eyes.at("eyes").at(index);
		view = JsonPose(entry.at("pose"));
		const auto& angles = entry.at("fov");
		fov = {angles.at(0).get<float>(), angles.at(1).get<float>(), angles.at(2).get<float>(),
			   angles.at(3).get<float>()};
	}
	else
	{
		const auto& views = configuration.at("views");
		const auto& angles = views.at("outer_fov");
		view = Compose(JsonPose(configuration.at("head")),
					   {{0, 0, 0, 1}, {(index ? .5f : -.5f) * views.at("ipd").get<float>(), 0, 0}});
		fov = {angles.at(index * 4).get<float>(), angles.at(index * 4 + 1).get<float>(),
			   angles.at(index * 4 + 2).get<float>(), angles.at(index * 4 + 3).get<float>()};
	}
	const auto target =
		ViewRayPoint(view, fov, {static_cast<float>(x), static_cast<float>(y)}, static_cast<float>(distance));
	const auto hand = command.at("hand").get<int>();
	const auto& controller = configuration.at("controllers").at(hand);
	const auto profile = controller.at("profile").get<std::string>();
	const auto grip = JsonPose(controller.at("pose"));
	const auto origin = controller.at("aim").is_null()
							? ControllerAimFromGrip({grip, TrackedPoseFlags, {}}, profile, hand == 0).pose.position
							: JsonPose(controller.at("aim")).position;
	const XrPosef aim{LookAt(origin, target), origin};
	Json result = PoseJson(ControllerGripFromAim(aim, profile, hand == 0), TrackedPoseFlags);
	result["kind"] = "controller_pose";
	result["hand"] = hand;
	result["connected"] = controller.at("connected");
	result["aim"] = PoseJson(aim, TrackedPoseFlags);
	return result;
}

static Json Command(const std::string& name, const Json& arguments)
{
	if (!arguments.is_object())
		throw std::invalid_argument("Arguments must be an object");
	auto command = arguments;
	for (const auto* key : {"connection", "session", "generation", "transaction_id"})
		command.erase(key);
	if (command.contains("kind") || command.contains("encoding"))
		throw std::invalid_argument("Control tools do not accept kind or encoding");
	command["kind"] = name.substr(4);
	return command;
}

std::vector<SimulationCommand> ControlsExpectedInputs(const std::string& name, const Json& configuration)
{
	if (name != "set_views" && name != "set_headset_profile")
		return {};
	auto views = configuration.at("views");
	views["kind"] = "views";
	return {SimulationCommandFromJson(views)};
}

std::vector<SimulationCommand> ControlsCommands(const std::string& name, const Json& arguments,
												const Json& configuration)
{
	auto command = Command(name, arguments);
	ControlChannel channel;
	if (name == "set_head_pose")
		channel = ControlChannel::Head;
	else if (name == "set_controller_pose" || name == "point_controller" || name == "set_controller_controls")
	{
		if (!command.contains("hand"))
			throw std::invalid_argument("hand is required");
		const auto& hand = command.at("hand");
		if (!hand.is_number_integer() || hand < 0 || hand > 1)
			throw std::invalid_argument("hand must be 0 (left) or 1 (right)");
		channel = name == "set_controller_controls"
					  ? (hand == 0 ? ControlChannel::LeftControls : ControlChannel::RightControls)
					  : (hand == 0 ? ControlChannel::LeftPose : ControlChannel::RightPose);
		if (name == "point_controller")
		{
			for (const auto& [key, value] : command.items())
				if (key != "kind" && key != "hand" && key != "x" && key != "y" && key != "eye" && key != "distance")
					throw std::invalid_argument("point_controller does not accept " + key);
			command = PointCommand(std::move(command), configuration);
		}
	}
	else if (name == "set_views" || name == "set_headset_profile")
	{
		channel = ControlChannel::Views;
		if (name == "set_headset_profile")
		{
			if (command.size() != 2 || !command.at("name").is_string())
				throw std::invalid_argument("Headset profile requires only name");
			const auto* profile = FindHeadsetProfile(command.at("name").get_ref<const std::string&>());
			if (!profile)
				throw std::invalid_argument("Unknown headset profile");
			const auto views = ProfileViews(*profile);
			Json fov = Json::array();
			for (const auto& eye : views.outerFov)
				for (const auto angle : {eye.angleLeft, eye.angleRight, eye.angleUp, eye.angleDown})
					fov.push_back(angle);
			command = {{"kind", "views"}, {"ipd", views.ipd}, {"outer_fov", std::move(fov)}};
		}
		else if (command.size() == 1)
			throw std::invalid_argument("set_views requires at least one view setting");
		auto views = configuration.at("views");
		views.update(command);
		command = std::move(views);
	}
	else
		throw std::invalid_argument("Unknown native control");
	return {TakeControl{channel, false}, SimulationCommandFromJson(command)};
}
} // namespace simxr::app::mcp
