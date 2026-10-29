#include "Pch.h"
#include "app/mcp/McpSimulation.h"
#include "app/mcp/McpGame.h"
#include "app/mcp/McpControls.h"
#include "control/ControlSchema.h"
#include "control/InspectionDocument.h"
#include "control/SimulationControl.h"
#include "simulation/BodyModel.h"
#include "simulation/HeadsetProfiles.h"
#include "../TestSupport.h"

using Json = nlohmann::json;
using namespace simxr;

static void Reject(const Json& command)
{
	try
	{
		(void)app::mcp::SimulationCommandFromJson(command);
	}
	catch (const std::exception&)
	{
		return;
	}
	Require(false, "Invalid typed command must be rejected");
}

static Json ObservedBody(BodyJointSet set)
{
	Json observation = {{"joint_set", static_cast<unsigned>(set)}, {"requested_time", "9007199254740993"},
		{"located", true}, {"joints", Json::array()}};
	const auto map = BodyModel::JointMap(set);
	const bool fb = set == BodyJointSet::UpperBodyFB || set == BodyJointSet::FullBodyMETA;
	if (fb)
	{
		observation["returned_time"] = "9007199254740991";
		observation["active"] = true;
		observation["confidence"] = 0.75;
		observation["skeleton_revision"] = 0;
		observation["skeleton"] = Json::array();
	}
	else
		observation["all_joint_poses_tracked"] = false;
	for (const auto joint : map)
	{
		const auto id = static_cast<unsigned>(joint);
		observation["joints"].push_back({{"joint", id}, {"position", {static_cast<float>(id), 0, 0}},
			{"orientation", {0, 0, 0, 1}}, {"flags", 1}});
		if (fb)
		{
			Json bone = {{"joint", id}, {"position", {0, 0, 0}}, {"orientation", {0, 0, 0, 1}}};
			if (joint != map[0])
				bone["parent"] = static_cast<unsigned>(map[0]);
			observation["skeleton"].push_back(std::move(bone));
		}
	}
	return observation;
}

int main()
{
	using app::mcp::SimulationCommandFromJson;
	Json configuration = {{"views", {{"ipd", .08}, {"outer_width", 1024}, {"outer_height", 768},
		{"inset_width", 320}, {"inset_height", 240}, {"inset_half_angle", .3},
		{"outer_fov", {-.8, .8, .8, -.8, -.8, .8, .8, -.8}}}}};
	const auto definitions = app::mcp::ControlsDefinitions();
	const auto profileTool = std::find_if(definitions.begin(), definitions.end(),
		[](const Json& value) { return value.at("name") == "set_headset_profile"; });
	Require(profileTool != definitions.end(), "Headset profile tool is advertised");
	const auto& names = profileTool->at("inputSchema").at("properties").at("name").at("enum");
	Require(names.size() == HeadsetProfiles().size(), "Every headset profile is advertised");
	for (const auto& profile : HeadsetProfiles())
	{
		Require(std::find(names.begin(), names.end(), profile.id) != names.end(), "Profile name is in schema");
		const Json arguments = {{"name", profile.id}};
		const auto commands = app::mcp::ControlsCommands("set_headset_profile", arguments, configuration);
		const auto expected = app::mcp::ControlsExpectedInputs("set_headset_profile", configuration);
		Require(commands.size() == 2 && expected.size() == 1 && std::abs(std::get<SetViewSettings>(expected[0]).value.ipd - .08f) < .00001f,
				"Profile edit takes view ownership and checks its observed baseline");
		const auto& command = commands[1];
		const auto& views = std::get<SetViewSettings>(command).value;
		const auto preset = ProfileViews(profile);
		Require(views.ipd == preset.ipd && views.outerFov[0].angleLeft == preset.outerFov[0].angleLeft &&
				views.outerFov[1].angleRight == preset.outerFov[1].angleRight && views.outerWidth == 1024 &&
				views.outerHeight == 768 && views.insetWidth == 320 && views.insetHeight == 240,
				"Profile optics use the shared table and preserve render dimensions");
	}
	bool badProfileRejected = false;
	try { (void)app::mcp::ControlsCommands("set_headset_profile", {{"name", "default"}}, configuration); }
	catch (const std::exception&) { badProfileRejected = true; }
	Require(badProfileRejected, "A legacy clear alias is not mistaken for a fixed generic preset");
	for (const auto& [name, command] : std::vector<std::pair<std::string, Json>>{
		{"set_head_pose", {{"kind", "head_pose"}, {"position", {1, 1.7, 2}}, {"orientation", {0, .70710678, 0, .70710678}}}},
		{"set_controller_pose", {{"kind", "controller_pose"}, {"hand", 1}, {"position", {1, 2, 3}}, {"connected", false}}},
		{"set_controller_controls", {{"kind", "controller_controls"}, {"hand", 0}, {"primary", true}, {"stick_x", -.2}}}})
	{
		auto arguments = command;
		arguments.erase("kind");
		const auto edit = app::mcp::ControlsCommands(name, arguments, configuration);
		Require(edit.size() == 2 && control::EncodeSimulationCommand(edit[1]).value ==
			control::EncodeSimulationCommand(SimulationCommandFromJson(command)).value,
			"Convenience controls and exact-frame commands share the native contract");
		Require(app::mcp::ControlsExpectedInputs(name, configuration).empty(),
			"Absolute replacements do not depend on unrelated applied inputs");
	}
	Reject({{"kind", "controller_pose"}});
	Reject({{"kind", "controller_controls"}, {"hand", -1}});
	Reject({{"kind", "controller_profile"}, {"path", "test"}});
	Reject({{"kind", "tracker"}, {"identity", "1"}});
	Reject({{"kind", "controller_controls"}, {"hand", 0}, {"primary", 1}});
	Reject({{"kind", "controller_controls"}, {"hand", 0}, {"trigger", 2}});
	Reject({{"kind", "controller_controls"}, {"hand", 0}, {"stick_y", -1.1}});
	Reject({{"kind", "head_pose"}, {"yaw_deg", 90}});
	const auto drain = app::mcp::PrepareSimulation("simulation_start_recording", {{"drain", true}}, 1, 2);
	Require(drain.opcode == control::OpCode::StartRecording, "Native recording start is exposed");
	const auto ack = app::mcp::PrepareSimulation("simulation_acknowledge_recording",
		{{"recording_id", "9007199254740993"}, {"through", "7"}}, 1, 2);
	const control::Message acknowledgement{.fields = ack.fields};
	Require(ack.opcode == control::OpCode::AcknowledgeRecording &&
		acknowledgement.Find(control::tag::RecordingId)->AsU64() == 9007199254740993ULL &&
		acknowledgement.Find(control::tag::Offset)->AsU64() == 7,
		"Recording acknowledgement preserves identity and exclusive offset");
	for (const auto set : {BodyJointSet::UpperBodyFB, BodyJointSet::FullBodyMETA,
						   BodyJointSet::WithoutArmsBD, BodyJointSet::FullBodyBD})
	{
		Json command = {{"kind", "body"}, {"preset", 2}, {"confidence", 0.75},
			{"controls", Json::array({{{"offset", {0.2, 0, 0}}}, Json::object(), Json::object(), Json::object(), Json::object()})},
			{"joints", Json::array({{{"joint", 2}, {"tracked", false}}})},
			{"observation", ObservedBody(set)}, {"observation_origin", "9007199254740990"}};
		const auto parsed = SimulationCommandFromJson(command);
		const auto encoded = control::EncodeSimulationCommand(parsed);
		const auto decoded = control::DecodeSimulationCommand(encoded);
		Require(control::EncodeSimulationCommand(decoded).value == encoded.value, "Body command round-trips through wire codec");
		const auto& body = std::get<SetBodyControls>(decoded);
		Require(body.value.preset == BodyPreset::Seated && body.value.controls[0].offset &&
				body.value.joints[2] && !body.value.joints[2]->tracked && body.observation &&
				body.observation->requestedTime.value.count() == 9007199254740993LL &&
				body.observationOrigin->value.count() == 9007199254740990LL, "Body values and timestamps survive");
		command["observation"]["joints"].push_back(command["observation"]["joints"][0]);
		Reject(command);
	}
	Json body = {{"kind", "body"}, {"observation", ObservedBody(BodyJointSet::UpperBodyFB)}};
	body["observation"]["skeleton"][0]["parent"] = body["observation"]["skeleton"][0]["joint"];
	Reject(body);
	Reject({{"kind", "body"}, {"observation_origin", "1"}});
	Reject({{"kind", "body"}, {"controls", Json::array({Json::object()})}});
	Reject({{"kind", "body"}, {"joints", Json::array({{{"joint", 2}}, {{"joint", 2}}})}});

	Json tracker = {{"kind", "tracker"}, {"index", 3}, {"identity", "18446744073709551615"}, {"role", 2},
		{"anchor", 5}, {"joint_offset", {{"position", {0.25, 0, 0}}}}, {"source_path", "test:/left-foot"}};
	const auto encoded = control::EncodeSimulationCommand(SimulationCommandFromJson(tracker));
	const auto decoded = control::DecodeSimulationCommand(encoded);
	const auto& device = std::get<SetTracker>(decoded);
	Require(device.index == 3 && device.value.identity.value == UINT64_MAX && device.value.anchor == BodyJoint::LeftFootAnkle &&
			device.value.jointOffset && device.value.sourcePath.View() == "test:/left-foot", "Tracker identity and data survive");
	Json inventory = {{"kind", "trackers"}, {"devices", Json::array({{{"identity", "1"}}, {{"identity", "2"}, {"index", 1}}})}};
	Require(std::get<SetTrackers>(SimulationCommandFromJson(inventory)).value.size() == 2, "Typed tracker inventory parses");
	tracker["identity"] = 9007199254740992ULL;
	Reject(tracker);
	tracker["identity"] = "01";
	Reject(tracker);
	tracker["identity"] = "1";
	tracker["position"] = {0, 0, 0};
	Reject(tracker);
	inventory["devices"][1]["index"] = 0;
	Reject(inventory);
	Reject({{"kind", "trackers"}, {"devices", Json::array({{{"identity", "1"}, {"extra", 1}}})}});

	Json eye = {{"kind", "eye_views"}, {"space_revision", "18446744073709551615"},
		{"left", {{"position", {-0.03, 1.6, 0}}, {"orientation", {0, 0, 0, 1}}, {"flags", 3}, {"fov", {-0.8, 0.9, 0.7, -0.6}}}},
		{"right", {{"position", {0.03, 1.6, 0}}, {"fov", {-0.9, 0.8, 0.7, -0.6}}}}};
	const auto eyeEncoded = control::EncodeSimulationCommand(SimulationCommandFromJson(eye));
	const auto eyeDecoded = control::DecodeSimulationCommand(eyeEncoded);
	const auto& observed = std::get<SetEyeViews>(eyeDecoded).value;
	Require(observed && observed->spaceRevision == UINT64_MAX && observed->poses[0].flags == 3 &&
			observed->fovs[1].angleRight == 0.8f && observed->poses[1].pose.position.x == 0.03f,
			"Eye observations and full-width revision survive wire encoding");
	Require(control::EncodeSimulationCommand(eyeDecoded).value == eyeEncoded.value, "Eye command round-trips");
	Require(!std::get<SetEyeViews>(SimulationCommandFromJson({{"kind", "eye_views"}, {"enabled", false}})).value,
			"Eye observations can be cleared");
	Reject({{"kind", "eye_views"}, {"enabled", false}, {"left", Json::object()}});
	Reject({{"kind", "eye_views"}, {"left", Json::object()}, {"right", Json::object()}});
	Reject({{"kind", "eye_views"}, {"left", {{"fov", {1, 2, 3, 4}}, {"unknown", true}}}, {"right", {{"fov", {1, 2, 3, 4}}}}});
	eye["space_revision"] = 9007199254740992ULL;
	Reject(eye);
	eye["space_revision"] = "18446744073709551616";
	Reject(eye);

	Json spaces = {{"kind", "reference_spaces"}, {"local_position", {1, 2, 3}},
		{"stage_orientation", {0, 0, 0, 1}}, {"stage_bounds", {3, 4}}};
	const auto spaceEncoded = control::EncodeSimulationCommand(SimulationCommandFromJson(spaces));
	const auto spaceDecoded = control::DecodeSimulationCommand(spaceEncoded);
	const auto& reference = std::get<SetReferenceSpaces>(spaceDecoded).value;
	Require(reference.local.position.y == 2 && reference.stageBounds && reference.stageBounds->width == 3 &&
			reference.stageBounds->height == 4, "Reference-space origins and bounds survive wire encoding");
	Require(control::EncodeSimulationCommand(spaceDecoded).value == spaceEncoded.value, "Reference command round-trips");
	const auto emptyBounds = SimulationCommandFromJson({{"kind", "reference_spaces"}, {"bounds_available", false}});
	Require(!std::get<SetReferenceSpaces>(emptyBounds).value.stageBounds, "Stage bounds can be unavailable");
	Reject({{"kind", "reference_spaces"}, {"bounds_available", false}, {"stage_bounds", {1, 0}}});
	Reject({{"kind", "reference_spaces"}, {"local_position", {0, 0}}});
	Reject({{"kind", "reference_spaces"}, {"stage_bounds", {2, 2}}, {"unknown", 1}});

	const auto gameCommand = [](Json arguments)
	{
		return app::mcp::PrepareGame("game_bridge_command", arguments, 12, 13);
	};
	Json request = {{"connection", "owner"}, {"game", "18446744073709551615"},
		{"command_id", "9007199254740993"}, {"kind", 1}, {"payload", std::string(4096, 'x')}};
	const auto prepared = gameCommand(request);
	Require(prepared.opcode == control::OpCode::SubmitGame, "Game command selects submit opcode");
	control::Message wire;
	wire.fields = prepared.fields;
	Require(wire.Find(control::tag::GameId)->AsU64() == UINT64_MAX &&
			wire.Find(control::tag::CommandId)->AsU64() == 9007199254740993ULL &&
			wire.Find(control::tag::Detail)->AsText()->size() == 4096, "Game command preserves identity and payload");
	const auto rejectedGame = [&](const Json& input)
	{
		try { (void)gameCommand(input); }
		catch (const std::exception&) { return; }
		Require(false, "Invalid game command must be rejected");
	};
	auto badGame = request;
	badGame["payload"] = std::string(4097, 'x');
	rejectedGame(badGame);
	badGame = request;
	badGame["game"] = 9007199254740992ULL;
	rejectedGame(badGame);
	badGame = request;
	badGame["kind"] = "1";
	rejectedGame(badGame);
	badGame = request;
	badGame["session"] = "12";
	rejectedGame(badGame);
	badGame["generation"] = "14";
	rejectedGame(badGame);
	badGame["generation"] = "13";
	Require(gameCommand(badGame).fields.size() == prepared.fields.size() + 2, "Matching session context is encoded");
	control::Message response;
	response.fields = {control::Field::U32(control::tag::Error, 0), control::Field::U64(control::tag::GameId, 0),
		control::Field::Bytes(control::tag::Document, control::InspectionValue::Object({}).Encode())};
	const auto gameStatus = app::mcp::GameReply(response, control::OpCode::GameStatus);
	Require(!gameStatus.at("isError").get<bool>(), "Unregistered game status permits game identity zero");
	bool incompleteRejected = false;
	try { (void)app::mcp::GameReply(response, control::OpCode::SubmitGame); }
	catch (const std::exception&) { incompleteRejected = true; }
	Require(incompleteRejected, "Incomplete mutation response is rejected");
	response.fields = {control::Field::U32(control::tag::Error, 0), control::Field::U64(control::tag::GameId, 1),
		control::Field::U64(control::tag::OperationId, 7), control::Field::Text(control::tag::Phase, "complete"),
		control::Field::I64(control::tag::NativeTime, 9007199254740993LL),
		control::Field::Bytes(control::tag::Document, control::InspectionValue::Object(
			{{"game_id", control::InspectionValue::Unsigned(1)}}).Encode())};
	const auto completed = app::mcp::GameReply(response, control::OpCode::GameOperation);
	const auto completedData = Json::parse(completed.at("content").at(0).at("text").get<std::string>());
	Require(completedData.at("document").at("game_id") == "1" &&
			completedData.at("native_time_ns") == "9007199254740993" && completedData.at("operation_id") == "7",
			"Game identity and clock fields retain exact precision");
	return 0;
}
