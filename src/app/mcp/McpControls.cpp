#include "app/mcp/McpPch.h"
#include "app/mcp/McpControls.h"
#include "app/mcp/McpSimulation.h"
#include "simulation/HeadsetProfiles.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;

bool ControlsHandles(const std::string& name)
{
	static const std::set<std::string> names{"set_head_pose", "set_controller_pose", "set_controller_controls",
		"set_views", "set_headset_profile", "get_headset_profiles"};
	return names.contains(name);
}

Json ControlsDefinitions()
{
	const Json text = {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}};
	const Json identifier = {{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^[1-9][0-9]{0,19}$"}},
		{{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991ULL}}})}};
	const Json number = {{"type", "number"}};
	const Json boolean = {{"type", "boolean"}};
	const Json hand = {{"type", "integer"}, {"enum", Json::array({0, 1})}, {"description", "0 left, 1 right; required."}};
	const auto vector = [&](unsigned size)
	{
		return Json{{"type", "array"}, {"minItems", size}, {"maxItems", size}, {"items", number}};
	};
	const Json common = {{"connection", text}, {"session", identifier}, {"generation", identifier}, {"transaction_id", identifier}};
	const auto tool = [](const char* name, const char* description, Json properties, Json required = Json::array())
	{
		required.push_back("connection");
		required.push_back("transaction_id");
		return Json{{"name", name}, {"description", std::string(description) +
			" Applies at the next frame with this connection's input ownership. Poll simulation_status for application; ownership or baseline conflicts reject the entire edit."},
			{"inputSchema", {{"type", "object"}, {"properties", std::move(properties)},
			{"required", std::move(required)}, {"additionalProperties", false}}}};
	};
	Json tools = Json::array();
	Json pose = {{"position", vector(3)}, {"orientation", vector(4)},
		{"flags", {{"type", "integer"}, {"minimum", 0}, {"maximum", 15}}}};
	auto head = common;
	head.update(pose);
	tools.push_back(tool("set_head_pose", "Replace head pose in stage space: position in metres, orientation quaternion [x,y,z,w]. Omitted components use zero position, identity orientation and flags 15.", head));
	auto controller = head;
	controller["hand"] = hand;
	controller["connected"] = boolean;
	controller["aim"] = {{"type", {"object", "null"}}, {"properties", pose}, {"additionalProperties", false}};
	tools.push_back(tool("set_controller_pose", "Replace controller grip pose in stage space using metres and quaternion [x,y,z,w], with optional independent aim pose. Defaults match head pose; connected defaults true.", controller, {"hand"}));
	auto controls = common;
	controls["hand"] = hand;
	for (const auto* key : {"trigger", "grip", "grip_force", "trackpad_force"})
		controls[key] = {{"type", "number"}, {"minimum", 0}, {"maximum", 1}};
	for (const auto* key : {"stick_x", "stick_y", "trackpad_x", "trackpad_y"})
		controls[key] = {{"type", "number"}, {"minimum", -1}, {"maximum", 1}};
	for (const auto* key : {"primary", "secondary", "menu", "back", "thumb_click", "trackpad_click", "primary_touch",
		"secondary_touch", "trigger_touch", "thumb_touch", "trackpad_touch", "thumbrest_touch", "connected"})
		controls[key] = boolean;
	controls["trigger_click"] = {{"type", {"boolean", "null"}}};
	controls["physical_profile"] = {{"type", "string"}, {"maxLength", 512}};
	tools.push_back(tool("set_controller_controls", "Replace held controller inputs. Omitted axes/buttons reset to zero/false; connected defaults true and trigger_click defaults to the runtime threshold. Buttons require JSON booleans.", controls, {"hand"}));
	auto views = common;
	views["ipd"] = number;
	views["outer_fov"] = vector(8);
	views["inset_half_angle"] = number;
	for (const auto* key : {"outer_width", "outer_height", "inset_width", "inset_height"})
		views[key] = {{"type", "integer"}, {"minimum", 1}, {"maximum", UINT32_MAX}};
	tools.push_back(tool("set_views", "Update view settings, preserving omitted fields. IPD uses metres; FOV angles use radians. outer_fov contains left/right/up/down for the left eye, then the right eye. Dimensions use pixels.", views));
	auto profile = common;
	Json names = Json::array();
	for (const auto& spec : HeadsetProfiles())
		names.push_back(spec.id);
	profile["name"] = {{"type", "string"}, {"enum", std::move(names)}};
	tools.push_back(tool("set_headset_profile", "Apply named optics, preserving render dimensions. The generic profile restores 0.064 metre IPD and symmetric pi/4 radian half angles.", profile, {"name"}));
	tools.push_back({{"name", "get_headset_profiles"},
		{"description", "List nominal headset optics in native units: ipd in metres, outer_fov in radians, recommended dimensions in pixels. No application is required."},
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
		profiles.push_back({{"name", spec.id}, {"ipd", views.ipd}, {"outer_fov", std::move(fovs)},
			{"recommended_width", views.outerWidth}, {"recommended_height", views.outerHeight}});
	}
	return {{"profiles", std::move(profiles)}};
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

std::vector<SimulationCommand> ControlsCommands(const std::string& name, const Json& arguments, const Json& configuration)
{
	auto command = Command(name, arguments);
	ControlChannel channel;
	if (name == "set_head_pose")
		channel = ControlChannel::Head;
	else if (name == "set_controller_pose" || name == "set_controller_controls")
	{
		const auto& hand = command.at("hand");
		if (!hand.is_number_integer() || hand < 0 || hand > 1)
			throw std::invalid_argument("hand must be 0 (left) or 1 (right)");
		channel = name == "set_controller_pose" ? (hand == 0 ? ControlChannel::LeftPose : ControlChannel::RightPose) :
			(hand == 0 ? ControlChannel::LeftControls : ControlChannel::RightControls);
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
