#include "app/mcp/McpPch.h"
#include "app/mcp/McpCompatibility.h"
#include "app/mcp/McpSimulation.h"
#include "math/Math.h"
#include "simulation/HeadsetProfiles.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;

bool CompatibilityHandles(const std::string& name)
{
	static const std::set<std::string> names{"set_head_pose", "set_controller_pose", "hold_controller_inputs", "set_ipd", "set_fov", "set_headset_profile", "get_headset_profiles"};
	return names.contains(name);
}

Json CompatibilityDefinitions()
{
	const Json text = {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}};
	const Json identifier = {{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^[1-9][0-9]{0,19}$"}},
		{{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991ULL}}})}};
	const Json number = {{"type", "number"}};
	const Json common = {{"connection", text}, {"session", identifier}, {"generation", identifier}, {"transaction_id", identifier}};
	const auto tool = [](const char* name, const char* description, Json properties)
	{
		return Json{{"name", name}, {"description", std::string(description) +
			" Poll simulation_status for application; a changed input or ownership conflict rejects the entire edit."}, {"inputSchema", {{"type", "object"},
			{"properties", std::move(properties)}, {"required", Json::array({"connection", "transaction_id"})},
			{"additionalProperties", false}}}};
	};
	Json tools = Json::array();
	auto head = common;
	for (const auto* key : {"x", "y", "z", "yaw_deg", "pitch_deg", "roll_deg"})
		head[key] = number;
	tools.push_back(tool("set_head_pose", "Set head pose in metres and degrees at the next frame using this connection's input ownership.", head));
	auto controller = common;
	for (const auto* key : {"hand", "posX", "posY", "posZ", "yaw", "pitch", "roll", "trigger"})
		controller[key] = number;
	tools.push_back(tool("set_controller_pose", "Set a controller pose in head-local metres and radians; hand 0 is left and 1 is right.", controller));
	auto controls = common;
	for (const auto* key : {"hand", "trigger", "grip", "primary", "secondary", "menu", "thumbClick", "stickX", "stickY"})
		controls[key] = number;
	controls["clear"] = {{"type", "boolean"}};
	tools.push_back(tool("hold_controller_inputs", "Update held controller inputs; hand -1 selects both hands. Omitted inputs retain their applied values.", controls));
	auto ipd = common;
	ipd["ipd_mm"] = number;
	ipd["clear"] = {{"type", "boolean"}};
	tools.push_back(tool("set_ipd", "Set IPD in millimetres or clear to 64 mm, preserving other applied view settings.", ipd));
	auto fov = common;
	fov["clear"] = {{"type", "boolean"}};
	Json eye = {{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}};
	for (const auto* key : {"left_deg", "right_deg", "up_deg", "down_deg"})
		eye["properties"][key] = number;
	fov["left_eye"] = eye;
	fov["right_eye"] = eye;
	tools.push_back(tool("set_fov", "Set both eye FOVs in degrees or clear to symmetric 45-degree angles, preserving other view settings.", fov));
	auto profile = common;
	Json profileNames = Json::array();
	for (const auto& spec : HeadsetProfiles())
		profileNames.push_back(spec.id);
	profile["name"] = {{"type", "string"}, {"enum", std::move(profileNames)}};
	auto definition = tool("set_headset_profile", "Set IPD and both eye FOVs from a named headset profile, preserving resolution and other view settings. Generic means symmetric 90-degree FOV and 64 mm IPD.", profile);
	definition["inputSchema"]["required"].push_back("name");
	tools.push_back(std::move(definition));
	tools.push_back({{"name", "get_headset_profiles"},
		{"description", "List the simulator's named headset profiles and their nominal optics; no running application is required."},
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
			fovs.push_back({eye.angleLeft, eye.angleRight, eye.angleUp, eye.angleDown});
		profiles.push_back({{"name", spec.id}, {"ipd_m", views.ipd}, {"fov_radians", std::move(fovs)},
			{"recommended_width", views.outerWidth}, {"recommended_height", views.outerHeight}});
	}
	return {{"profiles", std::move(profiles)}};
}

static void Keys(const Json& object, std::initializer_list<std::string_view> allowed)
{
	if (!object.is_object())
		throw std::invalid_argument("Arguments must be an object");
	for (auto it = object.begin(); it != object.end(); ++it)
		if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end())
			throw std::invalid_argument("Unknown argument: " + it.key());
}
static double Number(const Json& object, const char* key, double fallback)
{
	const auto it = object.find(key);
	if (it == object.end())
		return fallback;
	if (!it->is_number())
		throw std::invalid_argument(std::string(key) + " must be a number");
	const auto value = it->get<double>();
	if (!std::isfinite(value) || std::abs(value) > 1e6)
		throw std::invalid_argument(std::string(key) + " is out of range");
	return value;
}
static bool Clear(const Json& object)
{
	if (!object.contains("clear"))
		return false;
	if (!object["clear"].is_boolean())
		throw std::invalid_argument("clear must be boolean");
	return object["clear"].get<bool>();
}
static XrQuaternionf Orientation(double yaw, double pitch, double roll)
{
	const auto make = [](double angle, int axis)
	{
		const auto s = static_cast<float>(std::sin(angle * .5));
		return XrQuaternionf{axis == 0 ? s : 0.f, axis == 1 ? s : 0.f, axis == 2 ? s : 0.f,
			static_cast<float>(std::cos(angle * .5))};
	};
	return Multiply(Multiply(make(yaw, 1), make(pitch, 0)), make(roll, 2));
}
static Json OrientationJson(XrQuaternionf value)
{
	return Json::array({value.x, value.y, value.z, value.w});
}
static Json Take(const char* channel)
{
	return {{"kind", "take_control"}, {"channel", channel}};
}
static Json AppliedControls(const Json& configuration, int side)
{
	auto input = configuration.at("controllers").at(side).at("controls");
	input["stick_x"] = input.at("stick").at(0);
	input["stick_y"] = input.at("stick").at(1);
	input.erase("stick");
	input["trackpad_x"] = input.at("trackpad").at(0);
	input["trackpad_y"] = input.at("trackpad").at(1);
	input.erase("trackpad");
	input["kind"] = "controller_controls";
	input["hand"] = side;
	return input;
}
std::vector<SimulationCommand> CompatibilityExpectedInputs(const std::string& name, const Json& arguments, const Json& configuration)
{
	Json expected = Json::array();
	if (name == "set_controller_pose")
	{
		auto head = configuration.at("head");
		head["kind"] = "head_pose";
		expected.push_back(std::move(head));
	}
	if (name == "set_ipd" || name == "set_fov" || name == "set_headset_profile")
	{
		auto views = configuration.at("views");
		views["kind"] = "views";
		expected.push_back(std::move(views));
	}
	if ((name == "set_controller_pose" && arguments.contains("trigger")) ||
		(name == "hold_controller_inputs" && !Clear(arguments)))
	{
		const auto hand = Number(arguments, "hand", name == "set_controller_pose" ? 0 : -1);
		for (int side = 0; side < 2; ++side)
			if (hand == -1 || hand == side)
				expected.push_back(AppliedControls(configuration, side));
	}
	std::vector<SimulationCommand> result;
	result.reserve(expected.size());
	for (const auto& command : expected)
		result.push_back(SimulationCommandFromJson(command));
	return result;
}

std::vector<SimulationCommand> CompatibilityCommands(const std::string& name, const Json& arguments, const Json& configuration)
{
	if (!configuration.is_object())
		throw std::invalid_argument("Applied configuration is unavailable; advance an application frame");
	Json commands = Json::array();
	constexpr auto radians = std::numbers::pi / 180.0;
	if (name == "set_head_pose")
	{
		Keys(arguments, {"connection", "session", "generation", "transaction_id", "x", "y", "z", "yaw_deg", "pitch_deg", "roll_deg"});
		const auto q = Orientation(Number(arguments, "yaw_deg", 0) * radians, Number(arguments, "pitch_deg", 0) * radians,
			Number(arguments, "roll_deg", 0) * radians);
		commands = Json::array({Take("head"), {{"kind", "head_pose"}, {"position", Json::array({Number(arguments, "x", 0),
			Number(arguments, "y", 1.7), Number(arguments, "z", 0)})}, {"orientation", OrientationJson(q)}}});
	}
	else if (name == "set_ipd" || name == "set_fov" || name == "set_headset_profile")
	{
		if (name == "set_ipd")
			Keys(arguments, {"connection", "session", "generation", "transaction_id", "ipd_mm", "clear"});
		else if (name == "set_fov")
			Keys(arguments, {"connection", "session", "generation", "transaction_id", "left_eye", "right_eye", "clear"});
		else
			Keys(arguments, {"connection", "session", "generation", "transaction_id", "name"});
		const auto clear = Clear(arguments);
		auto views = configuration.at("views");
		views["kind"] = "views";
		if (name == "set_ipd")
		{
			const auto value = clear ? 64.0 : Number(arguments, "ipd_mm", 64);
			if (value < 0 || value > 200)
				throw std::invalid_argument("ipd_mm must be in [0, 200]");
			views["ipd"] = value / 1000;
		}
		else if (name == "set_fov")
		{
			Json fov = Json::array();
			for (const auto* side : {"left_eye", "right_eye"})
			{
				const auto eye = clear ? Json::object() : arguments.at(side);
				Keys(eye, {"left_deg", "right_deg", "up_deg", "down_deg"});
				for (const auto& [key, fallback] : {std::pair{"left_deg", -45.}, {"right_deg", 45.}, {"up_deg", 45.}, {"down_deg", -45.}})
					fov.push_back(Number(eye, key, fallback) * radians);
			}
			views["outer_fov"] = std::move(fov);
		}
		else
		{
			const auto& value = arguments.at("name");
			if (!value.is_string())
				throw std::invalid_argument("Headset profile name must be a string");
			const auto* spec = FindHeadsetProfile(value.get_ref<const std::string&>());
			if (!spec)
				throw std::invalid_argument("Unknown headset profile");
			const auto settings = ProfileViews(*spec);
			views["ipd"] = settings.ipd;
			Json fov = Json::array();
			for (const auto& eye : settings.outerFov)
				for (const auto angle : {eye.angleLeft, eye.angleRight, eye.angleUp, eye.angleDown})
					fov.push_back(angle);
			views["outer_fov"] = std::move(fov);
		}
		commands = Json::array({Take("views"), std::move(views)});
	}
	else if (name == "set_controller_pose" || name == "hold_controller_inputs")
	{
		const bool pose = name == "set_controller_pose";
		if (pose)
			Keys(arguments, {"connection", "session", "generation", "transaction_id", "hand", "posX", "posY", "posZ", "yaw", "pitch", "roll", "trigger"});
		else
			Keys(arguments, {"connection", "session", "generation", "transaction_id", "hand", "clear", "trigger", "grip", "primary", "secondary", "menu", "thumbClick", "stickX", "stickY"});
		const auto hand = Number(arguments, "hand", pose ? 0 : -1);
		if ((hand != 0 && hand != 1 && hand != -1) || (pose && hand == -1))
			throw std::invalid_argument("Invalid hand");
		for (int side = 0; side < 2; ++side)
		{
			if (hand != -1 && side != static_cast<int>(hand))
				continue;
			if (pose)
			{
				const auto& head = configuration.at("head");
				const auto& position = head.at("position");
				const auto& orientation = head.at("orientation");
				XrQuaternionf parent{orientation.at(0).get<float>(), orientation.at(1).get<float>(), orientation.at(2).get<float>(), orientation.at(3).get<float>()};
				const auto offset = Rotate(parent, {static_cast<float>(Number(arguments, "posX", side ? .2 : -.2)),
					static_cast<float>(Number(arguments, "posY", -.3)), static_cast<float>(Number(arguments, "posZ", -.4))});
				const auto q = Multiply(parent, Orientation(Number(arguments, "yaw", 0), Number(arguments, "pitch", 0), Number(arguments, "roll", 0)));
				commands.push_back(Take(side ? "right_pose" : "left_pose"));
				commands.push_back({{"kind", "controller_pose"}, {"hand", side}, {"position", Json::array({
					position.at(0).get<float>() + offset.x, position.at(1).get<float>() + offset.y, position.at(2).get<float>() + offset.z})}, {"orientation", OrientationJson(q)}});
			}
			if (!pose || arguments.contains("trigger"))
			{
				Json input = (!pose && Clear(arguments)) ? Json::object() : AppliedControls(configuration, side);
				input["kind"] = "controller_controls";
				input["hand"] = side;
				for (const auto& [key, output] : {std::pair{"trigger", "trigger"}, {"grip", "grip"}, {"stickX", "stick_x"}, {"stickY", "stick_y"},
					{"primary", "primary"}, {"secondary", "secondary"}, {"menu", "menu"}, {"thumbClick", "thumb_click"}})
				{
					if (!arguments.contains(key))
						continue;
					const auto value = Number(arguments, key, 0);
					const bool button = std::string_view(key) == "primary" || std::string_view(key) == "secondary" || std::string_view(key) == "menu" || std::string_view(key) == "thumbClick";
					const auto minimum = std::string_view(key).starts_with("stick") ? -1.0 : 0.0;
					if (value < minimum || value > 1 || (button && value != 0 && value != 1))
						throw std::invalid_argument(std::string(key) + " is out of range");
					if (button)
						input[output] = value != 0;
					else
						input[output] = value;
				}
				commands.push_back(Take(side ? "right_controls" : "left_controls"));
				commands.push_back(std::move(input));
			}
		}
	}
	else
		throw std::invalid_argument("Unknown compatibility control");
	std::vector<SimulationCommand> result;
	result.reserve(commands.size());
	for (const auto& command : commands)
		result.push_back(SimulationCommandFromJson(command));
	return result;
}
}
