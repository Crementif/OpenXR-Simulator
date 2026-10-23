#include "../TestSupport.h"
#include "control/FrameInspection.h"
#include "control/DebugProtocol.h"
#include "control/ControlSchema.h"
#include "simulation/TrackingSystem.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
using namespace simxr;
using namespace simxr::control;
using V = InspectionValue;
using namespace std::chrono_literals;

const Field& Find(const std::vector<Field>& fields, std::uint16_t tag)
{
	for (const auto& field : fields)
		if (field.tag == tag)
			return field;
	throw std::runtime_error("Missing inspection field");
}
std::vector<Field> Fields(std::span<const std::byte> bytes)
{
	auto decoded = DecodeFields(bytes);
	Require(decoded.error == DecodeError::None, "Invalid inspection document TLV");
	return std::move(decoded.fields);
}
std::vector<std::byte> Member(std::span<const std::byte> document, std::string_view name)
{
	const auto node = Fields(document);
	Require(Find(node, 2).AsU32() == static_cast<std::uint32_t>(InspectionType::Object), "Expected inspection object");
	for (const auto& child : Fields(Find(node, 3).value))
	{
		const auto fields = Fields(child.value);
		if (Find(fields, 1).AsText() == name)
			return child.value;
	}
	throw std::runtime_error("Missing inspection member");
}
std::uint64_t Unsigned(std::span<const std::byte> node)
{
	return *Find(Fields(node), 3).AsU64();
}
std::string Text(std::span<const std::byte> node)
{
	return std::string{*Find(Fields(node), 3).AsText()};
}

template <class F> void Capacity(F function)
{
	bool threw = false;
	try
	{
		function();
	}
	catch (const std::length_error&)
	{
		threw = true;
	}
	Require(threw, "Expected bounded inspection capacity failure");
}
void CatalogTransactionsAndReaders()
{
	InspectionStore store{2, 2, 64};
	const auto serialize = [](auto) { return std::vector<std::byte>(8); };
	const FrameKey key{{1}, {1}};
	auto first = store.Retain({1}, key, {{"shape", {std::byte{1}}}, {"shape", {std::byte{1}}}}, serialize);
	Require(first->id == 1 && first->revisions[0] == first->revisions[1] && first->revisions[0]->id == 1,
			"Repeated immutable content was not deduplicated");
	Require(store.Find({1}, key) == first && !store.Find({2}, key), "Session identity was omitted from inspection key");
	bool failed = false;
	try
	{
		store.Retain({1}, {{1}, {2}}, {{"different", {std::byte{2}}}},
					 [](auto) -> std::vector<std::byte> { throw std::runtime_error("serialization failed"); });
	}
	catch (const std::runtime_error&)
	{
		failed = true;
	}
	Require(failed && !store.Read(2) && !store.ReadRevision(2), "Failed publication exposed an inspection/revision");
	auto second = store.Retain({1}, {{1}, {2}}, {{"shape", {std::byte{1}}}, {"different", {std::byte{2}}}}, serialize);
	Require(second->id == 2 && second->revisions[0] == first->revisions[0] && second->revisions[1]->id == 2,
			"Failed publication consumed IDs or existing revision changed");
	Require(store.Release(1) && !store.Read(1) && !store.Release(1), "Logical inspection release failed");
	Capacity([&] { store.Retain({1}, {{1}, {3}}, {}, serialize); });
	first.reset();
	Capacity([&] { store.Retain({1}, {{1}, {3}}, {{"third", {std::byte{3}}}}, serialize); });
	Require(!store.Read(3), "Revision capacity failure published a frame");
	Require(store.Release(2), "Release second reader");
	auto third = store.Retain({1}, {{1}, {3}}, {{"shape", {std::byte{1}}}}, serialize);
	Require(third->id == 3 && third->revisions[0] == second->revisions[0],
			"Existing external revision reader did not participate in deduplication");
	Require(!store.ReadRevision(2), "Released revision remained newly discoverable");
	second.reset();
	third.reset();
	store.Release(3);
	Capacity([&] { store.Retain({1}, {{1}, {4}}, {}, [](auto) { return std::vector<std::byte>(65); }); });
	bool serializedOversizedFeatures = false;
	Capacity(
		[&]
		{
			store.Retain({1}, {{1}, {4}}, {{"oversized", std::vector<std::byte>(65)}},
						 [&](auto)
						 {
							 serializedOversizedFeatures = true;
							 return std::vector<std::byte>{};
						 });
		});
	Require(!serializedOversizedFeatures && !store.Read(4) && !store.ReadRevision(3),
			"Over-budget feature preparation reached serialization or published an identity");
	auto fourth = store.Retain({1}, {{1}, {4}}, {}, serialize);
	Require(fourth->id == 4, "Byte capacity failure consumed inspection ID");
}
FrameRecord Fixture()
{
	FrameRecord frame;
	frame.key = {{0xfedcba9876543210ULL}, {0x123456789abcdef0ULL}};
	frame.prediction.id = frame.key.frame;
	frame.prediction.displayTick = {42};
	frame.prediction.displayTime = {123ns};
	frame.prediction.displayPeriod = 11ms;
	frame.stage = FrameStage::Submitted;
	frame.submission.emplace();
	auto& state = *frame.submission;
	state.displayTime = {120ns};
	state.layerCount = 2;
	state.outputDisposition = FrameOutputDisposition::Submitted;
	state.input.revision = 99;
	state.input.time = {110ns};
	state.input.controllers[0].trigger = 0.25f;
	state.input.controllers[1].primary = true;
	TrackingSystem tracking;
	TrackingState input;
	input.head.pose.position.x = 0.75f;
	input.body.preset = BodyPreset::Seated;
	input.body.rootPose = XrPosef{{0, 1, 0, 0}, {2, 3, 4}};
	input.gaze.yaw = 0.2f;
	state.tracking = tracking.Publish({119ns}, input);
	{
		EnabledExtensions enabled;
		ActionRegistry registry{reinterpret_cast<XrInstance>(0xdeadbeef), enabled};
		XrPath right = XR_NULL_PATH, profile = XR_NULL_PATH, source = XR_NULL_PATH;
		Require(registry.StringToPath("/user/hand/right", right) == XR_SUCCESS &&
					registry.StringToPath("/interaction_profiles/oculus/touch_controller", profile) == XR_SUCCESS &&
					registry.StringToPath("/user/hand/right/input/a/click", source) == XR_SUCCESS,
				"Fixture paths");
		XrActionSetCreateInfo create{XR_TYPE_ACTION_SET_CREATE_INFO};
		std::strcpy(create.actionSetName, "historical");
		std::strcpy(create.localizedActionSetName, "Historical");
		create.priority = 41;
		XrActionSet set;
		Require(registry.CreateSet(create, set) == XR_SUCCESS, "Fixture action set");
		XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
		std::strcpy(actionInfo.actionName, "select");
		std::strcpy(actionInfo.localizedActionName, "Select");
		actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
		actionInfo.countSubactionPaths = 1;
		actionInfo.subactionPaths = &right;
		XrAction action;
		Require(registry.CreateAction(set, actionInfo, action) == XR_SUCCESS, "Fixture action");
		const XrActionSuggestedBinding binding{action, source};
		const XrInteractionProfileSuggestedBinding suggest{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr,
														   profile, 1, &binding};
		Require(registry.SuggestBindings(suggest) == XR_SUCCESS, "Fixture binding");
		ActionSyncState sync{registry};
		const XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &set};
		const XrActiveActionSet active{set, right};
		const XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
		Require(sync.Attach(attach) == XR_SUCCESS && sync.Sync(syncInfo, state.input, true) == XR_SUCCESS,
				"Fixture action synchronization");
		state.actions = sync.Snapshot();
		state.haptics.push_back({7,
								 {InputDeviceKind::Controller, 1},
								 action,
								 right,
								 0.5f,
								 120.f,
								 XR_MIN_HAPTIC_DURATION,
								 {100ns},
								 RuntimeTime{120ns},
								 HapticStatus::Stopped});
		Require(registry.DestroySet(set) == XR_SUCCESS, "Fixture retires live registry before export");
	}
	const auto source = reinterpret_cast<XrSwapchain>(0xf0e1d2c3b4a59687ULL);
	const auto depthSource = reinterpret_cast<XrSwapchain>(0x1020304050607080ULL);
	const auto space = reinterpret_cast<XrSpace>(0xabcdefab12345678ULL);
	const PoseSample pose{{{0, 0, 0, 1}, {0, 1, 0}}, TrackedPoseFlags, {117ns}};
	ProjectionLayerDescription projection;
	projection.space = {space, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, pose,
		XR_REFERENCE_SPACE_TYPE_LOCAL, pose.pose};
	ProjectionViewDescription view{
		pose.pose,
		pose,
		{-0.5f, 0.5f, 0.5f, -0.5f},
		{source, {{4, 8}, {12, 16}}, 2},
		LayerDepthDescription{{depthSource, {{1, 2}, {3, 4}}, 3}, 0, 1, 0.1f, std::numeric_limits<float>::infinity()}};
	projection.views = {view, view};
	QuadLayerDescription quad{projection.space, XR_EYE_VISIBILITY_BOTH,			pose.pose, pose,
							  {0.5f, 0.25f},	{source, {{0, 0}, {32, 32}}, 1}};
	state.layers = std::make_shared<const FrameLayers>(state.displayTime, state.blendMode,
													   std::vector<LayerDescription>{projection, quad});
	return frame;
}
void FrameStateAndRevisions()
{
	auto frame = Fixture();
	InspectionStore store;
	auto first = InspectFrame(store, {7}, frame);
	Require(first->revisions.size() == 3 &&
				Unsigned(Member(first->document, "generation")) == frame.key.generation.value &&
				Unsigned(Member(first->document, "frame_id")) == frame.key.frame.value,
			"Historical CPU identity/feature references changed");
	Require(Text(Member(Member(first->document, "availability"), "applied_foveation")) == "not_recorded",
			"Unrecorded state was claimed available");
	const auto viewRevision = Unsigned(Member(first->document, "view_settings"));
	const auto actionRevision = Unsigned(Member(Member(first->document, "actions"), "dictionary"));
	const auto skeletonRevision = Unsigned(Member(Member(Member(first->document, "tracking"), "body"), "skeleton"));
	const auto bodyRoot = [](const auto& document)
	{
		return Member(Member(Member(Member(document, "tracking"), "input"), "body"), "root_pose");
	};
	const auto root = bodyRoot(first->document);
	Require(Find(Fields(Member(Member(root, "position"), "x")), 3).AsU32() == std::bit_cast<std::uint32_t>(2.f) &&
			Find(Fields(Member(Member(root, "orientation"), "y")), 3).AsU32() == std::bit_cast<std::uint32_t>(1.f),
			"Historical body root pose was not preserved");
	const auto original = first->document;
	frame.key.frame.value++;
	frame.prediction.id = frame.key.frame;
	frame.submission->input.controllers[0].trigger = 0.75f;
	frame.submission->tracking->head.pose.position.x = -1;
	frame.submission->tracking->input.body.rootPose.reset();
	auto second = InspectFrame(store, {7}, frame);
	Require(Find(Fields(bodyRoot(second->document)), 2).AsU32() == static_cast<std::uint32_t>(InspectionType::Null) &&
			bodyRoot(first->document) == root, "Absent body root or retained historical pose changed");
	Require(first->document == original && Unsigned(Member(second->document, "view_settings")) == viewRevision &&
				Unsigned(Member(Member(second->document, "actions"), "dictionary")) == actionRevision &&
				Unsigned(Member(Member(Member(second->document, "tracking"), "body"), "skeleton")) == skeletonRevision,
			"Input/tracking mutation changed historical state or duplicated static feature records");
	frame.key.frame.value++;
	frame.submission->viewSettings.ipd = 0.07f;
	auto third = InspectFrame(store, {7}, frame);
	Require(Unsigned(Member(third->document, "view_settings")) != viewRevision && first->document == original,
			"Changed view configuration reused an immutable revision");
	frame.key.frame.value++;
	frame.submission->tracking->proportions.height = 2.0f;
	frame.submission->tracking->body.skeleton[0].bindPose.position.z = 0.2f;
	const auto changedSkeleton = InspectFrame(store, {7}, frame);
	Require(Unsigned(Member(Member(Member(changedSkeleton->document, "tracking"), "body"), "skeleton")) !=
					skeletonRevision &&
				first->document == original,
			"Changed skeleton content reused a revision solely because its model counter matched");
	const auto& encoded = first->document;
	const auto raw = Field::U64(1, 0xf0e1d2c3b4a59687ULL).value;
	Require(std::search(encoded.begin(), encoded.end(), raw.begin(), raw.end()) == encoded.end(),
			"Native source handle escaped in CPU document");
}
void DepthEncodingPreservesNonfiniteBits()
{
	const auto firstElement = [](std::span<const std::byte> array)
	{
		const auto pages = Fields(Find(Fields(array), 3).value);
		return Fields(pages.at(0).value).at(0).value;
	};
	for (const auto bits : {0x7f800000U, 0xff800000U, 0x7fc01234U, 0x80000000U})
	{
		auto frame = Fixture();
		auto projection = std::get<ProjectionLayerDescription>(frame.submission->layers->layers.front());
		projection.views.front().depth->nearZ = std::bit_cast<float>(bits);
		projection.views.front().depth->farZ = std::bit_cast<float>(bits);
		frame.submission->layers = std::make_shared<const FrameLayers>(
			frame.submission->displayTime, frame.submission->blendMode, std::vector<LayerDescription>{projection});
		InspectionStore store;
		const auto record = InspectFrame(store, {7}, frame);
		const auto layer = firstElement(Member(Member(record->document, "layers"), "layers"));
		const auto view = firstElement(Member(layer, "views"));
		const auto depth = Member(view, "depth");
		for (const auto name : {"near_z", "far_z"})
		{
			const auto value = Fields(Member(depth, name));
			Require(Find(value, 2).AsU32() == static_cast<std::uint32_t>(InspectionType::Float) &&
					Find(value, 3).AsU32() == bits, "Historical depth changed a float's type, sign or NaN payload");
		}
	}
}

void FrameHistoryBytePages()
{
	FrameHistorySnapshot history;
	history.generation = {1};
	history.capacity = 128;
	auto applied = std::make_shared<AppliedCommandSnapshot>();
	for (auto& joint : applied->configuration.tracking.body.joints)
		joint = BodyJointOverride{XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true};
	auto frame = Fixture();
	frame.submission->commands = applied;
	for (std::uint64_t id = 1; id <= 128; ++id)
	{
		frame.key = {history.generation, {id}};
		history.frames.push_back(std::make_shared<const FrameRecord>(frame));
	}
	std::uint64_t after = 0;
	unsigned pages = 0;
	while (after < 128)
	{
		Message response;
		response.requestId = {1};
		EncodeFrameHistory(response, history, after, 128);
		Require(Encode(response).size() <= HeaderSize + MaxPayload, "Frame page exceeds wire budget");
		const auto frames = DecodeFields(response.Find(tag::Frames)->value);
		Require(frames.error == DecodeError::None && !frames.fields.empty(), "History page made no progress");
		for (const auto& entry : frames.fields)
		{
			const auto fields = Fields(entry.value);
			Require(Find(fields, tag::FrameId).AsU64() == ++after, "Frame pagination skipped or repeated history");
			Require(!Find(fields, tag::AppliedInput).value.empty(), "Frame pagination dropped input metadata");
		}
		++pages;
	}
	Require(pages > 1, "Large frame history did not exercise byte pagination");
}
void ProjectionHistoryBoundaries()
{
	FrameHistorySnapshot history;
	history.generation = Fixture().key.generation;
	history.capacity = 128;
	history.evicted = 7;
	const auto first = Fixture().key.frame.value;
	for (std::uint64_t index = 0; index < 64; ++index)
	{
		auto frame = Fixture();
		frame.key.frame.value += index;
		auto projection = std::get<ProjectionLayerDescription>(frame.submission->layers->layers.front());
		projection.views.resize(4, projection.views.front());
		frame.submission->layers = std::make_shared<const FrameLayers>(frame.submission->displayTime,
			frame.submission->blendMode, std::vector<LayerDescription>{projection, QuadLayerDescription{}, projection});
		history.frames.push_back(std::make_shared<const FrameRecord>(std::move(frame)));
	}
	const auto document = InspectProjectionHistory(history, 3).Encode();
	Require(Unsigned(Member(document, "generation")) == history.generation.value &&
		Unsigned(Member(document, "evicted_frames")) == 7 && Unsigned(Member(document, "omitted_entries")) == 125,
		"Projection history omitted its full-width identity or bounded retention evidence");
	const auto pages = Fields(Find(Fields(Member(document, "entries")), 3).value);
	const auto entries = Fields(pages.front().value);
	Require(entries.size() == 3 && Unsigned(Member(entries[0].value, "frame_id")) == first + 62 &&
		Unsigned(Member(entries[0].value, "layer")) == 2 &&
		Unsigned(Member(entries[1].value, "frame_id")) == first + 63 &&
		Unsigned(Member(entries[1].value, "layer")) == 0 && Unsigned(Member(entries[2].value, "layer")) == 2,
		"Projection history changed submitted frame/layer order or included a quad");
	const auto bounded = InspectProjectionHistory(history, 64).Encode();
	Require(bounded.size() < MaxPayload - 1024, "Maximum projection history exceeds the wire response budget");
	const auto raw = Field::U64(1, 0xf0e1d2c3b4a59687ULL).value;
	Require(std::search(bounded.begin(), bounded.end(), raw.begin(), raw.end()) == bounded.end(),
		"Projection history exposed a native source handle");
	for (const auto count : {0U, 65U})
	{
		bool rejected = false;
		try
		{
			InspectProjectionHistory(history, count);
		}
		catch (const std::invalid_argument&)
		{
			rejected = true;
		}
		Require(rejected, "Projection history accepted an unbounded request");
	}
}
void ArrayBoundaries()
{
	std::vector<V> values;
	for (unsigned index = 0; index < 300; ++index)
		values.push_back(V::Unsigned(index));
	const auto document = V::Array(std::move(values)).Encode();
	const auto pages = Fields(Find(Fields(document), 3).value);
	Require(pages.size() == 2 && Fields(pages[0].value).size() == 256 && Fields(pages[1].value).size() == 44,
			"Inspection array truncated at TLV field bound");
}
} // namespace
int main(int argc, char** argv)
{
	try
	{
		if (argc == 2 && std::string_view{argv[1]} == "--fixture")
		{
			InspectionStore store;
			const auto record = InspectFrame(store, {7}, Fixture());
			std::vector<V> revisions;
			for (const auto& revision : record->revisions)
				revisions.push_back(V::Object(
					{{"revision_id", V::Unsigned(revision->id)},
					 {"kind", V::Text(revision->document.kind)},
					 {"document", {InspectionType::Object, Find(Fields(revision->document.bytes), 3).value}}}));
			const auto bytes = V::Object({{"frame", {InspectionType::Object, Find(Fields(record->document), 3).value}},
										  {"revisions", V::Array(std::move(revisions))}})
								   .Encode();
			std::cout.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			return 0;
		}
		CatalogTransactionsAndReaders();
		FrameStateAndRevisions();
		ArrayBoundaries();
		ProjectionHistoryBoundaries();
		FrameHistoryBytePages();
		DepthEncodingPreservesNonfiniteBits();
		std::cout << "Immutable inspection transactions, revision deduplication, reader capacity and historical state "
					 "passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
