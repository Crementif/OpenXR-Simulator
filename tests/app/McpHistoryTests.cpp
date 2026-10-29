#include "Pch.h"
#include "app/mcp/McpHistory.h"
#include "app/mcp/McpIncidentSave.h"
#include "app/mcp/McpSequence.h"
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

static void IncidentArchiveRollback()
{
	const auto root = std::filesystem::temp_directory_path() /
		("simxr-incident-rollback-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directory(root);
	try
	{
		for (const unsigned stop : {3u, 5u})
		{
			const auto archive = root / std::to_string(stop);
			Json metadata = {{"window", {{"entries", Json::array({{{"frame_id", "1"}, {"phase", "ready"}}})}}}};
			const std::vector<std::vector<std::byte>> frames{std::vector<std::byte>(320 * 104 * 4)};
			unsigned checks = 0;
			bool cancelled = false;
			try
			{
				app::mcp::SaveIncident(archive, metadata, {}, frames, 320, 104, [&]
				{
					if (++checks == stop)
						throw std::runtime_error("cancel archive");
				});
			}
			catch (const std::runtime_error& error)
			{
				cancelled = std::string_view(error.what()) == "cancel archive";
			}
			Require(cancelled && !std::filesystem::exists(archive),
					"Cancelled incident retained published files or temporary output");
		}
	}
	catch (...)
	{
		std::filesystem::remove_all(root);
		throw;
	}
	std::filesystem::remove(root);
}

int main()
{
	IncidentArchiveRollback();
	const auto sequenceId = UINT64_MAX;
	wire::Message sequenceResponse;
	sequenceResponse.fields = {wire::Field::U32(tag::Error, 0), wire::Field::U64(tag::SequenceId, sequenceId),
		wire::Field::Bytes(tag::Document, wire::InspectionValue::Object({
			{"id", wire::InspectionValue::Unsigned(sequenceId)}, {"active", wire::InspectionValue::Boolean(true)}}).Encode())};
	const app::mcp::SequenceRequest sequenceRequest{wire::OpCode::StartSequence, {}};
	const auto sequence = app::mcp::SequenceDocument(sequenceResponse, sequenceRequest);
	Require(sequence.at("sequence_id").is_number_unsigned() && sequence.at("sequence_id") == sequenceId &&
		sequence.at("document").at("id") == sequenceId, "Internal sequence IDs preserve all 64 bits as integers");
	const auto sequenceReply = app::mcp::SequenceReply(sequenceResponse, sequenceRequest);
	const auto published = Json::parse(sequenceReply.at("content").at(0).at("text").get<std::string>());
	Require(published.at("sequence_id") == std::to_string(sequenceId) &&
		published.at("document").at("id") == std::to_string(sequenceId), "Public sequence IDs remain decimal strings");
	for (const auto& invalidId : {wire::InspectionValue::Unsigned(sequenceId - 1), wire::InspectionValue::Signed(-1)})
	{
		sequenceResponse.fields.back() = wire::Field::Bytes(tag::Document,
			wire::InspectionValue::Object({{"id", invalidId}}).Encode());
		bool rejected = false;
		try { (void)app::mcp::SequenceDocument(sequenceResponse, sequenceRequest); }
		catch (const std::exception&) { rejected = true; }
		Require(rejected, "Sequence response cannot substitute a different or signed execution ID");
	}
	const auto special = app::mcp::InspectionJson(wire::InspectionValue::Object({
		{"far_z", wire::InspectionValue::Float(std::numeric_limits<float>::infinity())},
		{"negative", wire::InspectionValue::Float(-std::numeric_limits<float>::infinity())},
		{"unknown", wire::InspectionValue::Float(std::numeric_limits<float>::quiet_NaN())}}).Encode());
	Require(special.at("far_z") == "Infinity" && special.at("negative") == "-Infinity" && special.at("unknown") == "NaN",
		"Non-finite inspection values remain explicit in valid JSON");
	wire::Message projection;
	projection.fields = {wire::Field::Bytes(tag::Document, wire::InspectionValue::Object({
		{"generation", wire::InspectionValue::Unsigned(UINT64_MAX)},
		{"entries", wire::InspectionValue::Array({wire::InspectionValue::Object({})})}}).Encode())};
	Require(app::mcp::ProjectionHistoryDocument(projection, 1, UINT64_MAX).at("generation") == std::to_string(UINT64_MAX),
		"Projection metadata preserves its exact generation");
	for (const auto& [count, generation] : {std::pair{0u, UINT64_MAX}, std::pair{1u, UINT64_MAX - 1}})
	{
		bool rejected = false;
		try { (void)app::mcp::ProjectionHistoryDocument(projection, count, generation); }
		catch (const std::exception&) { rejected = true; }
		Require(rejected, "Projection diagnostics reject excess entries or a changed generation");
	}
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
