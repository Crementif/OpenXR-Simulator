#include "Pch.h"
#include "app/mcp/McpHistory.h"
#include "app/mcp/McpSimulation.h"
#include "control/ControlSchema.h"
#include "control/DebugProtocol.h"
#include "control/InspectionDocument.h"
#include "presentation/FrameHistory.h"
#include "simulation/BodyModel.h"
#include "../TestSupport.h"

using namespace simxr;
using Json = nlohmann::json;
namespace wire = simxr::control;
namespace tag = simxr::control::tag;

int main()
{
	const auto special = app::mcp::InspectionJson(wire::InspectionValue::Object({
		{"far_z", wire::InspectionValue::Float(std::numeric_limits<float>::infinity())},
		{"negative", wire::InspectionValue::Float(-std::numeric_limits<float>::infinity())},
		{"unknown", wire::InspectionValue::Float(std::numeric_limits<float>::quiet_NaN())}}).Encode());
	Require(special.at("far_z") == "Infinity" && special.at("negative") == "-Infinity" && special.at("unknown") == "NaN",
		"Non-finite inspection values remain explicit in valid JSON");
	const Json arguments = {{"connection", "owner"}, {"after", "9007199254740992"}, {"count", 2}};
	const auto request = app::mcp::PrepareHistory("debug_frame_history", arguments, 7, 8);
	Require(request.after == 9007199254740992ULL && request.count == 2, "Exact cursor survives preparation");
	for (const auto& invalid : {Json{{"connection", "owner"}, {"after", "18446744073709551616"}},
		Json{{"connection", "owner"}, {"after", "00"}}, Json{{"connection", "owner"}, {"count", 0}},
		Json{{"connection", "owner"}, {"count", 129}}})
	{
		bool rejected = false;
		try { (void)app::mcp::PrepareHistory("debug_frame_history", invalid, 7, 8); }
		catch (const std::exception&) { rejected = true; }
		Require(rejected, "Invalid history request is rejected");
	}
	auto applied = std::make_shared<AppliedCommandSnapshot>();
	applied->seed = 9007199254740993ULL;
	applied->configuration.tracking.head.pose.position.x = 2.5f;
	applied->configuration.input.controllers[0].trigger = .75f;
	applied->configuration.tracking.trackers = {{{9007199254740993ULL}, TrackerRole::LeftFoot}};
	applied->configuration.tracking.eyeViews.emplace();
	applied->configuration.tracking.eyeViews->spaceRevision = 9007199254740993ULL;
	BodyObservation observation;
	observation.requestedTime = {Duration{9007199254740993LL}};
	for (const auto joint : BodyModel::JointMap(observation.jointSet))
		observation.joints[static_cast<std::size_t>(joint)] = ObservedBodyJoint{};
	applied->configuration.tracking.bodyObservation = observation;
	FrameHistorySnapshot history;
	history.generation = {8};
	history.capacity = 128;
	history.origin = {Duration{9007199254740993LL}};
	for (std::uint64_t index = 0; index < 2; ++index)
	{
		auto frame = std::make_shared<FrameRecord>();
		frame->key = {{8}, {9007199254740993ULL + index}};
		frame->prediction.id = frame->key.frame;
		frame->prediction.displayTick = {9007199254740995ULL + index};
		frame->prediction.displayTime = {Duration{9007199254740997LL + static_cast<std::int64_t>(index)}};
		frame->prediction.displayPeriod = Duration{11000000};
		frame->stage = FrameStage::Submitted;
		frame->submission.emplace();
		frame->submission->displayTime = frame->prediction.displayTime;
		frame->submission->commands = applied;
		history.frames.push_back(std::move(frame));
	}
	wire::Message response;
	response.fields.push_back(wire::Field::U32(tag::Error, 0));
	wire::EncodeFrameHistory(response, history, request.after, 2, true);
	const auto result = app::mcp::HistoryReply(response, request, 8);
	Require(!result.at("isError").get<bool>(), "Valid history response accepted");
	const auto data = Json::parse(result.at("content").at(0).at("text").get<std::string>());
	Require(data.at("origin_time_ns") == "9007199254740993" && data.at("next_after") == "9007199254740994" &&
		data.at("frames").size() == 2, "Frame cursor and time preserve full-width precision");
	const auto& first = data.at("frames").at(0);
	Require(first.at("prediction").at("tick") == "9007199254740995" &&
		first.at("prediction").at("time_ns") == "9007199254740997", "Prediction identity and time remain exact");
	const auto& input = first.at("applied_input");
	Require(input.at("seed") == "9007199254740993" && input.at("configuration").at("head").at("position").at(0) == 2.5 &&
		input.at("configuration").at("trackers").at(0).at("id") == "9007199254740993" &&
		input.at("configuration").at("eye_views").at("space_revision") == "9007199254740993" &&
		!input.at("configuration").contains("focused"), "Historical state is readable without inventing focus");
	const auto& commands = input.at("commands");
	const auto body = app::mcp::SimulationCommandFromJson(commands.at(5));
	Require(std::get<SetBodyControls>(body).observation &&
		std::get<SetBodyControls>(body).observation->requestedTime.value.count() == 9007199254740993LL,
		"Canonical body observation retains its full-width provider time");
	const auto trackers = app::mcp::SimulationCommandFromJson(commands.back());
	Require(std::get<SetTrackers>(trackers).value[0].identity.value == 9007199254740993ULL,
		"Canonical historical command can be resubmitted without losing tracker identity");
	auto broken = response;
	auto list = wire::DecodeFields(broken.Find(tag::Frames)->value);
	auto firstFrame = wire::DecodeFields(list.fields[0].value);
	firstFrame.fields.push_back(wire::Field::U32(65535, 1, true));
	list.fields[0].value = wire::EncodeFields(firstFrame.fields);
	for (auto& field : broken.fields)
		if (field.tag == tag::Frames)
			field.value = wire::EncodeFields(list.fields);
	bool unknownRejected = false;
	try { (void)app::mcp::HistoryReply(broken, request, 8); }
	catch (const std::exception&) { unknownRejected = true; }
	Require(unknownRejected, "Unknown required historical frame fields are not silently ignored");
	response.fields.erase(std::remove_if(response.fields.begin(), response.fields.end(),
		[](const auto& field) { return field.tag == tag::Frames; }), response.fields.end());
	bool rejected = false;
	try { (void)app::mcp::HistoryReply(response, request, 8); }
	catch (const std::exception&) { rejected = true; }
	Require(rejected, "Missing history page is rejected");
	return 0;
}
