#include "../TestSupport.h"
#include "app/services/FrameHistoryFeed.h"
#include "app/services/HistoryStore.h"
#include "control/SimulationControl.h"
#include "simulation/CommandState.h"
#include "simulation/BodyModel.h"
#include "math/Math.h"
#include <chrono>
#include <iostream>
#include <limits>
#include <set>

namespace
{
using namespace simxr::app;

void RingAndBuckets()
{
	HistoryStore history(37);
	for (std::uint64_t i = 0; i < 120; ++i)
		history.Append({{4, i / 60 + 1, i % 60},
						7,
						0,
						double(i),
						float(i % 11),
						i == 100			 ? HistoryLate | HistoryDropped
						: i == 20 || i == 90 ? HistoryDropped
											 : 0U});
	Require(history.Size() == 37 && history.At(0).seconds == 83, "History ring did not evict oldest frames");
	for (std::size_t first = 0; first <= history.Size(); ++first)
		for (std::size_t end = first; end <= history.Size(); ++end)
		{
			const auto result = history.Query(first, end);
			Require(result.count == end - first && (first != end || result.missed == 0),
					"Wrapped bucket count differs from frame range");
			if (first == end)
				continue;
			float minimum = 100, maximum = -1;
			std::uint32_t flags = 0;
			std::size_t missed = 0;
			for (auto index = first; index < end; ++index)
			{
				minimum = std::min(minimum, history.At(index).frameMilliseconds);
				maximum = std::max(maximum, history.At(index).frameMilliseconds);
				flags |= history.At(index).flags;
				missed += (history.At(index).flags & (HistoryLate | HistoryDropped)) != 0;
			}
			Require(result.minimum == minimum && result.maximum == maximum && result.flags == flags &&
						result.missed == missed,
					"Bucket lost a one-frame peak or event across ring wrap");
		}
	Require(history.Find({4, 2, 40}, 7) == 17, "Identity lookup lost wrapped frame");
	Require(!history.Find({4, 2, 1}, 7), "Identity lookup retained evicted frame");
	Require(history.LatestSegment({4, 2, 0}, 7) == std::pair<std::size_t, std::size_t>{0, 37} &&
				history.Latest({4, 2, 0}, 7) == 36 && !history.LatestSegment({4, 1, 0}, 7) &&
				!history.LatestSegment({4, 2, 0}, 8) && !history.LatestSegment({5, 2, 0}, 7),
			"Latest segment includes evicted frames or another scope");
	history.SetFlags(17, HistoryMarker, true);
	Require(history.Query(0, 37).flags & HistoryMarker, "Marker absent from density buckets");
	history.SetFlags(17, HistoryMarker, false);
	history.SetCoverage(17, 89, HistorySnapshot);
	Require(history.At(17).identity.frame == 40 && history.At(17).identity.run == 2 && history.At(17).epoch == 7,
			"Coverage changed frame identity");
	Require(history.Query(0, 37).flags == (HistoryLate | HistoryDropped | HistorySnapshot) &&
				history.Query(0, 37).missed == 2,
			"Coverage lost timing flags or counted a late and dropped frame twice");
	history.SetCoverage(17, 0, 0);
	Require(history.Query(0, 37).flags == (HistoryLate | HistoryDropped),
			"Released image coverage remained in ancestor buckets");
	history.SetFlags(17, HistoryLate, false);
	Require(history.Query(0, 37).missed == 2, "Clearing lateness removed a still-dropped frame from the miss count");
	history.SetFlags(17, HistoryDropped, false);
	Require(history.Query(0, 37).missed == 1, "Cleared miss remained in ancestor buckets");
	history.SetFlags(0, HistoryLate | HistoryDropped | HistoryDriven, true);
	Require(history.Query(0, 37).missed == 2 && history.Query(0, 37).driven == 1 && history.Query(1, 37).driven == 0,
			"Updated miss or driven frame was omitted or counted twice");
	Require(history.Nearest(99.8) == 17 && history.LowerBound(99.8) == 17, "Time hit-testing selected wrong frame");
	Require(history.Nearest(-1) == 0 && history.Nearest(1000) == 36, "Time hit-testing did not clamp edges");
	history.Append({{4, 3, 1}, 7, 0, 120, 10, HistoryRunStart});
	Require(history.LatestSegment({4, 2, 0}, 7) == std::pair<std::size_t, std::size_t>{0, 36} &&
				history.LatestSegment({4, 3, 0}, 7) == std::pair<std::size_t, std::size_t>{36, 37} &&
				history.Query(0, 37).missed == 1 && history.Query(0, 37).driven == 0,
			"New run merged segment bounds or retained an overwritten miss");
	history.Append({{4, 2, 60}, 7, 0, 121, 10, HistoryRunStart});
	Require(history.LatestSegment({4, 2, 0}, 7) == std::pair<std::size_t, std::size_t>{36, 37},
			"Returning to a run included its older segment");
	Require(history.SegmentAt(0) == std::pair<std::size_t, std::size_t>{0, 35} &&
				history.SegmentAt(35) == std::pair<std::size_t, std::size_t>{35, 36} &&
				history.SegmentAt(36) == std::pair<std::size_t, std::size_t>{36, 37},
			"Segment lookup merged a revisited scope with its original retained segment");
	history.Append({{4, 2, 61}, 7, 0, 122, 10, HistoryRunStart});
	Require(history.LatestSegment({4, 2, 0}, 7) == std::pair<std::size_t, std::size_t>{36, 37},
			"Explicit run boundary joined separate observations of the same run");
	history.Clear();
	Require(!history.Nearest(0) && !history.LatestSegment({4, 2, 0}, 7) && history.Query(0, 0).count == 0 &&
				history.Query(0, 0).missed == 0 && history.Query(0, 0).driven == 0,
			"Clear retained old buckets or segments");
	bool rejected = false;
	try
	{
		history.Append({{}, 0, 0, std::numeric_limits<double>::quiet_NaN()});
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected && history.Size() == 0, "Invalid time damaged history");
}

void RuntimeFeed()
{
	namespace wire = simxr::control;
	namespace tag = wire::tag;
	const auto page = [](std::uint64_t run, std::uint64_t id, std::int64_t time, std::uint64_t frameRun = 0) {
		wire::Message message;
		const std::vector<wire::Field> prediction{wire::Field::I64(tag::Time, time),
												  wire::Field::I64(tag::Period, 10000000)};
		const std::vector<wire::Field> frame{wire::Field::U64(tag::Generation, frameRun ? frameRun : run),
											 wire::Field::U64(tag::FrameId, id),
											 wire::Field::Bytes(tag::Prediction, wire::EncodeFields(prediction)),
											 wire::Field::Text(tag::Stage, "discarded")};
		const std::vector<wire::Field> frames{wire::Field::Bytes(tag::Frame, wire::EncodeFields(frame))};
		message.fields = {wire::Field::U64(tag::Session, 9), wire::Field::U64(tag::Generation, run),
						  wire::Field::Bytes(tag::Frames, wire::EncodeFields(frames)), wire::Field::I64(tag::Time, 0)};
		return message;
	};
	FrameHistoryFeed feed;
	feed.SetScope({1, 9, 2});
	feed.Accept(page(2, 10, 100000000));
	feed.Accept(page(2, 10, 100000000));
	feed.Accept(page(2, 12, 120000000));
	Require(feed.History().Size() == 2 && feed.After() == 12, "Feed repeated a paginated frame");
	Require(feed.History().At(1).flags == (HistoryDropped | HistoryGap | HistoryLate) &&
				feed.History().At(1).frameMilliseconds == 20 && feed.History().At(1).periodMilliseconds == 10,
			"Feed lost a dropped frame, gap, late frame or runtime interval");
	Require(feed.History().Query(0, 2).missed == 2 &&
				feed.History().LatestSegment({9, 2, 0}, 1) == std::pair<std::size_t, std::size_t>{0, 2},
			"Runtime segment double-counted lateness or lost its retained bounds");
	Require(std::abs(feed.History().At(1).runSeconds - .12) < .000001, "Run-relative time used attachment time");
	feed.SetScope({});
	feed.SetScope({1, 9, 2});
	feed.Accept(page(2, 12, 120000000));
	Require(feed.History().Size() == 2, "Scope revisit duplicated retained frames");
	feed.SetScope({2, 9, 3});
	feed.Accept(page(2, 13, 130000000));
	Require(feed.After() == 0, "Old run response advanced new run cursor");
	feed.Accept(page(3, 1, 1000));
	Require(feed.History().Size() == 3 && feed.History().At(2).epoch == 2 && feed.History().At(2).identity.run == 3 &&
				(feed.History().At(2).flags & HistoryRunStart) &&
				feed.History().At(2).seconds > feed.History().At(1).seconds,
			"Run clocks were joined without a divider");
	Require(feed.History().LatestSegment({9, 2, 0}, 1) == std::pair<std::size_t, std::size_t>{0, 2} &&
				feed.History().LatestSegment({9, 3, 0}, 2) == std::pair<std::size_t, std::size_t>{2, 3} &&
				!feed.History().LatestSegment({9, 3, 0}, 1),
			"Run transition changed the retained segment identity");
	Require(!feed.Accept(page(3, 2, 2000, 4)).empty() && feed.After() == 2, "Invalid history page was not skipped");
	Require(feed.Accept(page(3, 3, 3000)).empty() && feed.History().Size() == 4 &&
				(feed.History().At(3).flags & HistoryGap),
			"Frame after a skipped page lacks its gap");
	feed.SetScope({3, 9, 4});
	auto invalid = page(4, 1, 1000000000);
	auto frames = wire::DecodeFields(invalid.Find(tag::Frames)->value).fields;
	auto frame = wire::DecodeFields(frames.front().value).fields;
	frame.push_back(wire::Field::Bytes(tag::AppliedInput, std::vector<std::byte>{std::byte{1}}));
	frames.front().value = wire::EncodeFields(frame);
	for (auto& field : invalid.fields)
		if (field.tag == tag::Frames)
			field.value = wire::EncodeFields(frames);
	Require(!feed.Accept(invalid).empty(), "Malformed first-frame input was accepted");
	const auto previous = feed.History().At(feed.History().Size() - 1).seconds;
	Require(feed.Accept(page(4, 2, 1010000000)).empty(), "Valid frame after malformed input rejected");
	const auto& recovered = feed.History().At(feed.History().Size() - 1);
	Require(std::abs(recovered.seconds - previous - .1) < .000001 && (recovered.flags & HistoryGap),
			"Rejected first frame changed the next run's timeline origin");
	auto broken = page(4, 3, 1020000000);
	for (auto& field : broken.fields)
		if (field.tag == tag::Frames)
			field.value = {std::byte{1}};
	Require(!feed.Accept(broken).empty() && !feed.CanRead() && feed.After() == 2,
			"Undecodable history list must stop polling without throwing or inventing a cursor");
	Require(feed.Accept(broken).empty(), "Unreadable page repeated its notice");
	feed.SetScope({4, 9, 4});
	Require(feed.CanRead() && feed.Accept(page(4, 3, 1020000000)).empty(), "Reconnect did not resume history");
	broken = page(4, 4, 1030000000);
	for (auto& field : broken.fields)
		if (field.tag == tag::Frames)
			field.value = wire::EncodeFields(std::vector{wire::Field::Bytes(1, std::vector{std::byte{1}})});
	Require(!feed.Accept(broken).empty() && !feed.CanRead() && feed.After() == 3,
			"Undecodable record identity must stop polling without throwing from recovery");
}

void AppliedInputFeed()
{
	namespace wire = simxr::control;
	namespace tag = wire::tag;
	using namespace simxr;
	const auto frame = [](std::uint64_t id, bool reported, std::size_t trackerCount = 4, bool collection = false,
						  std::optional<BodyObservation> observation = {}) {
		TrackingState tracking;
		tracking.body.preset = id < 100 ? BodyPreset::Standing : BodyPreset::TPose;
		tracking.body.rootPose = XrPosef{{0, 0, 0, 1}, {0, 0, -0.01f * float(id)}};
		tracking.gaze.yaw = id < 200 ? 0 : .25f;
		tracking.trackers[0].poseFlags = id < 250 ? TrackedPoseFlags : XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
		tracking.trackers[0].poseOverride = XrPosef{{0, 0, 0, 1}, {0, 1, -0.01f * float(id == 250 ? 49 : id % 50)}};
		std::vector<wire::Field> applied{wire::EncodeSimulationCommand(SetHeadPose{tracking.head}, 1)};
		if (reported)
		{
			for (std::uint32_t hand = 0; hand < 2; ++hand)
			{
				applied.push_back(wire::EncodeSimulationCommand(SetControllerPose{hand, tracking.controllers[hand]},
																static_cast<std::uint16_t>(2 + hand)));
				applied.push_back(wire::EncodeSimulationCommand(SetControllerControls{hand, {}},
																static_cast<std::uint16_t>(4 + hand)));
			}
			applied.push_back(wire::EncodeSimulationCommand(SetBodyControls{tracking.body, observation}, 6));
			applied.push_back(wire::EncodeSimulationCommand(SetGaze{tracking.gaze}, 7));
			if (collection)
			{
				tracking.trackers.clear();
				for (std::size_t slot = 0; slot < trackerCount; ++slot)
					tracking.trackers.push_back({{100 + slot}, TrackerRole::Unassigned});
				applied.push_back(
					wire::EncodeSimulationCommand(SetTrackers{tracking.trackers}, tag::TrackerCollection));
			}
			else
				for (std::uint32_t slot = 0; slot < 4; ++slot)
					applied.push_back(wire::EncodeSimulationCommand(SetTracker{slot, tracking.trackers[slot]},
																	static_cast<std::uint16_t>(8 + slot)));
		}
		ViewSettings views;
		views.ipd = id < 150 ? .06f : .07f;
		std::vector<wire::Field> context{
			wire::EncodeSimulationCommand(SetViewSettings{views}, 1),
			wire::EncodeSimulationCommand(SetBodyProportions{}, 2),
			wire::EncodeSimulationCommand(SetControllerProfile{0, id < 150 ? "first" : "second"}, 3),
			wire::EncodeSimulationCommand(SetControllerProfile{1, "right"}, 4),
			wire::EncodeSimulationCommand(SetReferenceSpaces{}, 5)};
		if (id < 200)
		{
			StereoEyeViews eyes;
			eyes.spaceRevision = id < 150 ? 1 : 2;
			eyes.poses[0].flags = TrackedPoseFlags;
			eyes.poses[0].pose.position.x = id < 150 ? -.03f : -.04f;
			eyes.fovs[1] = {-.7f, .8f, .6f, -.9f};
			context.push_back(wire::EncodeSimulationCommand(SetEyeViews{eyes}, 6));
		}
		applied.push_back(wire::Field::Bytes(tag::InputContext, wire::EncodeFields(context)));
		for (const auto name : {tag::Seed, tag::Scenario, tag::Ownership, tag::ActiveActions})
			applied.push_back(wire::Field::U32(name, 0));
		applied[applied.size() - 4] = wire::Field::U64(tag::Seed, 5);
		const std::vector<wire::Field> prediction{wire::Field::I64(tag::Time, std::int64_t(id) * 11000000),
												  wire::Field::I64(tag::Period, 11000000)};
		const std::vector<wire::Field> fields{wire::Field::U64(tag::Generation, 2), wire::Field::U64(tag::FrameId, id),
											  wire::Field::Bytes(tag::Prediction, wire::EncodeFields(prediction)),
											  wire::Field::Text(tag::Stage, "presented"),
											  wire::Field::Bytes(tag::AppliedInput, wire::EncodeFields(applied))};
		return wire::Field::Bytes(tag::Frame, wire::EncodeFields(fields));
	};
	const auto page = [&](std::uint64_t first, std::uint64_t count, bool reported) {
		std::vector<wire::Field> frames;
		for (auto id = first; id < first + count; ++id)
		{
			frames.push_back(frame(id, reported));
			frames.back().tag = static_cast<std::uint16_t>(frames.size());
		}
		wire::Message message;
		message.fields = {wire::Field::U64(tag::Session, 9), wire::Field::U64(tag::Generation, 2),
						  wire::Field::Bytes(tag::Frames, wire::EncodeFields(frames)), wire::Field::I64(tag::Time, 0)};
		return message;
	};
	for (const auto set :
		 {BodyJointSet::UpperBodyFB, BodyJointSet::FullBodyMETA, BodyJointSet::WithoutArmsBD, BodyJointSet::FullBodyBD})
	{
		BodyObservation observation;
		observation.jointSet = set;
		observation.requestedTime = {Duration{9007199254740993LL}};
		observation.located = true;
		const bool fb = set == BodyJointSet::UpperBodyFB || set == BodyJointSet::FullBodyMETA;
		if (fb)
		{
			observation.returnedTime = RuntimeTime{Duration{9007199254740991LL}};
			observation.active = false;
			observation.confidence = 0;
			observation.skeletonRevision = 0;
			observation.skeleton.emplace();
		}
		else
			observation.allJointPosesTracked = false;
		for (const auto joint : BodyModel::JointMap(set))
		{
			const auto index = static_cast<std::size_t>(joint);
			observation.joints[index] = ObservedBodyJoint{IdentityPose(), 0};
			if (fb)
				(*observation.skeleton)[index] = SkeletonJoint{joint, {}, IdentityPose()};
		}
		FrameHistoryFeed observedFeed;
		observedFeed.SetScope({1, 9, 2});
		auto observedFrame = frame(1, true, 4, false, observation);
		observedFrame.tag = 1;
		wire::Message observedPage;
		observedPage.fields = {wire::Field::U64(tag::Session, 9), wire::Field::U64(tag::Generation, 2),
							   wire::Field::Bytes(tag::Frames, wire::EncodeFields(std::vector{observedFrame})),
							   wire::Field::I64(tag::Time, 0)};
		Require(observedFeed.Accept(observedPage).empty(), "Observed body frame was rejected");
		const auto* first = observedFeed.Input().Find(observedFeed.History().At(0).input);
		Require(first && first->complete && first->bodyObservation &&
					wire::EncodeSimulationCommand(SetBodyControls{{}, *first->bodyObservation}).value ==
						wire::EncodeSimulationCommand(SetBodyControls{{}, observation}).value,
				"Frame ingestion changed provider body data");
		InputHistory observedHistory(4);
		auto sample = *first;
		const auto one = observedHistory.Append(sample);
		sample.identity.frame++;
		sample.bodyObservation = std::make_shared<const BodyObservation>(observation);
		const auto two = observedHistory.Append(sample);
		Require(observedHistory.Find(one)->bodyObservation == observedHistory.Find(two)->bodyObservation,
				"Identical observations did not share history storage");
		observation.requestedTime.value += Duration{1};
		sample.identity.frame++;
		sample.bodyObservation = std::make_shared<const BodyObservation>(observation);
		const auto three = observedHistory.Append(sample);
		sample.identity.frame++;
		sample.bodyObservation.reset();
		const auto four = observedHistory.Append(sample);
		const auto observedTake = TakeFromInput({observedHistory.Find(one), observedHistory.Find(two),
												 observedHistory.Find(three), observedHistory.Find(four)});
		Require(observedTake.version == 4 && observedTake.transactions.size() == 2,
				"Body observation timestamp or source-clear transition was lost");
		const auto& changedBody = std::get<SetBodyControls>(observedTake.transactions[0].commands[0]);
		const auto& clearedBody = std::get<SetBodyControls>(observedTake.transactions[1].commands[0]);
		Require(changedBody.observation && changedBody.observation->requestedTime == observation.requestedTime &&
					!clearedBody.observation,
				"Take changed observation time or failed to restore authored body");
		Require(!automation::EditableRig(observedTake).empty(), "Observed history take cannot export");
		std::vector<InputSample> variants(4, *first);
		std::vector<const InputSample*> variantRange;
		for (std::size_t index = 0; index < variants.size(); ++index)
		{
			variants[index].identity.frame = index + 1;
			auto value = *first->bodyObservation;
			if (fb)
			{
				if (index >= 1)
					value.skeletonRevision = 1;
				if (index >= 2)
					value.skeleton->at(static_cast<std::size_t>(BodyModel::JointMap(set)[0]))->bindPose.position.x = 1;
				if (index >= 3)
					value.confidence = .5f;
			}
			else
			{
				if (index >= 1)
					value.allJointPosesTracked = true;
				if (index >= 2)
					value.joints[static_cast<std::size_t>(BodyModel::JointMap(set)[0])]->flags = ValidPoseFlags;
				if (index >= 3)
					value.joints[static_cast<std::size_t>(BodyModel::JointMap(set)[0])]->pose.position.x = 1;
			}
			variants[index].bodyObservation = std::make_shared<const BodyObservation>(value);
			variantRange.push_back(&variants[index]);
		}
		Require(TakeFromInput(variantRange).transactions.size() == 3,
				"Take thinned provider metadata, skeleton, validity or pose changes");
		std::weak_ptr<const BodyObservation> retained = observedHistory.Find(three)->bodyObservation;
		observedHistory.Clear();
		Require(retained.expired(), "Cleared history retained provider body storage");
	}

	std::cout << "Observed body feed, take transitions and storage release passed\n";
	FrameHistoryFeed feed;
	feed.SetScope({1, 9, 2});
	constexpr std::uint64_t Frames = 32768;
	for (std::uint64_t first = 1; first <= Frames; first += 128)
		Require(feed.Accept(page(first, 128, true)).empty(), "Applied input page rejected");
	const auto& history = feed.History();
	std::vector<const InputSample*> samples;
	for (std::size_t index = 0; index < history.Size(); ++index)
		samples.push_back(feed.Input().Find(history.At(index).input));
	Require(samples.size() == Frames && samples.back() && samples.back()->body && samples.back()->trackers,
			"Applied input lost body or trackers");
	Require(!samples.front() && samples.back(), "Byte eviction did not retire oldest input references");
	std::set<const BodyControls*> bodies;
	std::size_t retained = 0;
	for (const auto* sample : samples)
		if (sample)
		{
			++retained;
			bodies.insert(sample->body.get());
		}
	Require(bodies.size() == 1 && retained < Frames && feed.RetainedBytes() <= 64 * 1024 * 1024,
			"History failed to share body blocks or respect its combined CPU budget");
	std::cout << retained << " retained input samples within " << feed.RetainedBytes() << " CPU history bytes\n";
	FrameHistoryFeed rangeFeed;
	rangeFeed.SetScope({1, 9, 2});
	for (std::uint64_t first = 1; first <= 260; first += 128)
		Require(rangeFeed.Accept(page(first, std::min<std::uint64_t>(128, 261 - first), true)).empty(),
				"Short take fixture rejected");
	std::vector<const InputSample*> range;
	for (std::size_t index = 50; index < 260; ++index)
		range.push_back(rangeFeed.Input().Find(rangeFeed.History().At(index).input));

	const auto take = TakeFromInput(range);
	bool initialEyes = false, changedEyes = false, clearedEyes = false;
	bool initialViews = false, initialProfile = false, changedViews = false, changedProfile = false;
	for (const auto& command : take.initial)
	{
		if (const auto* eyes = std::get_if<SetEyeViews>(&command))
			initialEyes = eyes->value && eyes->value->spaceRevision == 1 &&
						  eyes->value->poses[0].pose.position.x == -.03f && eyes->value->fovs[1].angleDown == -.9f;
		if (const auto* views = std::get_if<SetViewSettings>(&command))
			initialViews = views->value.ipd == .06f;
		if (const auto* profile = std::get_if<SetControllerProfile>(&command); profile && profile->hand == 0)
			initialProfile = profile->path == "first";
	}
	for (const auto& transaction : take.transactions)
		for (const auto& command : transaction.commands)
		{
			if (const auto* eyes = std::get_if<SetEyeViews>(&command))
			{
				changedEyes |=
					eyes->value && eyes->value->spaceRevision == 2 && eyes->value->poses[0].pose.position.x == -.04f;
				clearedEyes |= !eyes->value;
			}
			if (const auto* views = std::get_if<SetViewSettings>(&command))
				changedViews |= views->value.ipd == .07f;
			if (const auto* profile = std::get_if<SetControllerProfile>(&command); profile && profile->hand == 0)
				changedProfile |= profile->path == "second";
		}
	Require(initialEyes && changedEyes && clearedEyes,
			"Cut take preserves observed eyes, calibration changes and removal");
	Require(initialViews && initialProfile && changedViews && changedProfile,
			"Cut take lost its recorded view/profile context or changes");
	const auto initialBody = std::find_if(take.initial.begin(), take.initial.end(), [](const auto& command) {
		return std::holds_alternative<SetBodyControls>(command);
	});
	Require(initialBody != take.initial.end() &&
				std::get<SetBodyControls>(*initialBody).value.preset == BodyPreset::Standing &&
				std::get<SetBodyControls>(*initialBody).value.rootPose,
			"Take origin lost the recorded body");
	bool tpose = false, tracker = false, gaze = false, partialTracker = false;
	for (const auto& transaction : take.transactions)
		for (const auto& command : transaction.commands)
		{
			if (const auto* body = std::get_if<SetBodyControls>(&command))
				tpose |= body->value.preset == BodyPreset::TPose && transaction.frame.value == 100;
			if (const auto* device = std::get_if<SetTrackers>(&command))
			{
				tracker |= !device->value.empty() && device->value[0].poseOverride;
				partialTracker |= !device->value.empty() &&
								  device->value[0].poseFlags == XR_SPACE_LOCATION_ORIENTATION_VALID_BIT &&
								  transaction.frame.value == 250;
			}
			if (const auto* value = std::get_if<SetGaze>(&command))
				gaze |= value->value.yaw == .25f && transaction.frame.value == 200;
		}
	Require(tpose && tracker && gaze && partialTracker, "Cut take lacks body, tracker or gaze changes");

	FrameHistoryFeed collections;
	collections.SetScope({1, 9, 2});
	std::vector<wire::Field> collectionFrames;
	for (const auto count : {64u, 0u, 5u})
	{
		auto value = frame(collectionFrames.size() + 1, true, count, true);
		value.tag = static_cast<std::uint16_t>(collectionFrames.size() + 1);
		collectionFrames.push_back(std::move(value));
	}
	wire::Message collectionPage;
	collectionPage.fields = {wire::Field::U64(tag::Session, 9), wire::Field::U64(tag::Generation, 2),
							 wire::Field::Bytes(tag::Frames, wire::EncodeFields(collectionFrames)),
							 wire::Field::I64(tag::Time, 0)};
	Require(collections.Accept(collectionPage).empty(), "Collection frame metadata was rejected");
	std::vector<const InputSample*> collectionSamples;
	for (std::size_t index = 0; index < collections.History().Size(); ++index)
		collectionSamples.push_back(collections.Input().Find(collections.History().At(index).input));
	Require(collectionSamples.size() == 3 && collectionSamples[0]->complete &&
				collectionSamples[0]->trackers->size() == 64 && collectionSamples[1]->trackers->empty() &&
				collectionSamples[2]->trackers->size() == 5,
			"Collection history lost topology transitions");
	const auto collectionTake = TakeFromInput(collectionSamples);
	const auto trackersOf = [](const auto& transaction) -> const TrackerDevices& {
		for (const auto& command : transaction.commands)
			if (const auto* value = std::get_if<SetTrackers>(&command))
				return value->value;
		throw std::runtime_error("Missing collection transition");
	};
	Require(collectionTake.version == 3 && collectionTake.transactions.size() == 2 &&
				trackersOf(collectionTake.transactions[0]).empty() &&
				trackersOf(collectionTake.transactions[1]).size() == 5,
			"Take omitted inventory removal or restoration");

	std::vector<InputSample> edges(1026, *range.front());
	std::vector<const InputSample*> edgeRange;
	for (std::size_t index = 0; index < edges.size(); ++index)
	{
		edges[index].identity.frame = 1000 + index;
		edges[index].controls[0].primary = (index % 2) != 0;
		edgeRange.push_back(&edges[index]);
	}
	const auto refuses = [](const std::vector<const InputSample*>& input) {
		try
		{
			TakeFromInput(input);
		}
		catch (const std::invalid_argument&)
		{
			return true;
		}
		return false;
	};
	Require(refuses(edgeRange), "Long take silently thinned button edges");
	edgeRange.pop_back();
	const auto boundary = TakeFromInput(edgeRange);
	Require(boundary.transactions.size() == 1024, "Take lost a transition at the transaction limit");
	for (std::size_t index = 0; index < boundary.transactions.size(); ++index)
	{
		const auto& transaction = boundary.transactions[index];
		Require(transaction.frame.value == 1001 + index && transaction.commands.size() == 1 &&
					std::get<SetControllerControls>(transaction.commands.front()).value.primary == ((index % 2) == 0),
				"Take changed a button edge or its frame");
	}
	edges[1].controls = edges[0].controls;
	edges[2].controls = edges[0].controls;
	edges[1].poses[0].aim = PoseSample{};
	edges[1].poses[0].aim->pose.position.x = .25f;
	edges[1].poses[0].aim->flags = TrackedPoseFlags;
	const auto aimTake = TakeFromInput({&edges[0], &edges[1], &edges[2]});
	Require(aimTake.transactions.size() == 2 &&
				std::get<SetControllerPose>(aimTake.transactions[0].commands.front()).value.aim->pose.position.x ==
					.25f &&
				!std::get<SetControllerPose>(aimTake.transactions[1].commands.front()).value.aim,
			"Cut take retains aim-only changes and return to legacy grip-derived aim");
	edges[1].poses[0].aim.reset();
	edges[1].controls[0].triggerClick = false;
	const auto clickTake = TakeFromInput({&edges[0], &edges[1], &edges[2]});
	Require(
		clickTake.transactions.size() == 2 &&
			std::get<SetControllerControls>(clickTake.transactions[0].commands.front()).value.triggerClick == false &&
			!std::get<SetControllerControls>(clickTake.transactions[1].commands.front()).value.triggerClick.has_value(),
		"Cut take retains explicit false trigger click and return to analog fallback");
	edges[1].controls[0].triggerClick.reset();
	edges[1].controls[0].physicalProfile = "/interaction_profiles/htc/vive_controller";
	const auto profileTake = TakeFromInput({&edges[0], &edges[1], &edges[2]});
	Require(
		profileTake.transactions.size() == 2 &&
			std::get<SetControllerControls>(profileTake.transactions[0].commands.front()).value.physicalProfile ==
				"/interaction_profiles/htc/vive_controller" &&
			std::get<SetControllerControls>(profileTake.transactions[1].commands.front()).value.physicalProfile.empty(),
		"Cut take retains profile-only changes and return to simulated preference");
	edges[1].controls[0].physicalProfile.clear();
	edges[1].scenarios[static_cast<std::size_t>(FailureScenario::HeadUntracked)] = true;
	const auto scenarios = TakeFromInput({&edges[0], &edges[1], &edges[2]});
	Require(scenarios.transactions.size() == 2 &&
				std::get<SetScenario>(scenarios.transactions[0].commands.front()).enabled &&
				!std::get<SetScenario>(scenarios.transactions[1].commands.front()).enabled,
			"Take omitted scenario activation or recovery");
	edges[1].scenarios = edges[0].scenarios;
	const auto recenter = static_cast<std::size_t>(FailureScenario::Recenter);
	edges[1].scenarios[recenter] = true;
	edges[1].head.pose.position = {7, 1.8f, -3};
	edges[1].head.flags = TrackedPoseFlags;
	auto context = wire::DecodeFields(*edges[1].context).fields;
	ReferenceSpaceState recordedSpaces;
	recordedSpaces.local.position = {2, 1.6f, -1};
	recordedSpaces.stage.position = {2, 0, -1};
	for (auto& field : context)
	{
		const auto command = wire::DecodeSimulationCommand(field);
		if (std::holds_alternative<SetReferenceSpaces>(command))
			field = wire::EncodeSimulationCommand(SetReferenceSpaces{recordedSpaces}, field.tag);
		else if (const auto* profile = std::get_if<SetControllerProfile>(&command))
			field = wire::EncodeSimulationCommand(
				SetControllerProfile{profile->hand, "/interaction_profiles/khr/simple_controller"}, field.tag);
	}
	edges[1].context = std::make_shared<const std::vector<std::byte>>(wire::EncodeFields(context));
	const auto recenteredTake = TakeFromInput({&edges[0], &edges[1]});
	Require(recenteredTake.transactions.size() == 1, "Recentered range lost its boundary");
	CommandState replayState;
	CommandConfiguration baseline;
	CommandTransaction replay;
	replay.source = {CommandSourceKind::Rig, 9};
	for (std::size_t index = 0; index < ControlChannelCount; ++index)
		replay.commands.push_back(TakeControl{static_cast<ControlChannel>(index)});
	const auto& changes = recenteredTake.transactions.front().commands;
	replay.commands.insert(replay.commands.end(), changes.begin(), changes.end());
	Require(replayState.Apply(replay, baseline, RuntimeTime{Duration{1}}) == CommandResult::Applied &&
				SameReferenceSpaces(replayState.Merge(baseline).referenceSpaces, recordedSpaces),
			"Range replay recomputed the captured recenter origin from the final head pose");
	edges[0].context = edges[1].context;
	edges[1].head = edges[0].head;
	const auto unchangedRecenter = TakeFromInput({&edges[0], &edges[1]});
	Require(unchangedRecenter.transactions.size() == 1, "Unchanged recenter lost its frame");
	bool restoresSpaces = false;
	for (const auto& command : unchangedRecenter.transactions.front().commands)
	{
		restoresSpaces |= std::holds_alternative<SetReferenceSpaces>(command);
		Require(!std::holds_alternative<SetScenario>(command),
				"Snapshot recenter was exported as another computed event");
	}
	Require(restoresSpaces, "Unchanged recenter lost the reference-space notification");
	auto cleared = edges[1];
	cleared.identity.frame++;
	cleared.scenarios[recenter] = false;
	auto repeated = cleared;
	repeated.identity.frame++;
	repeated.scenarios[recenter] = true;
	const auto repeatedTake = TakeFromInput({&edges[0], &edges[1], &cleared, &repeated});
	Require(repeatedTake.transactions.size() == 2 &&
				repeatedTake.transactions.back().frame.value == repeated.identity.frame,
			"A consumed recenter reset hid a subsequent equal-origin recenter");
	const auto clearedTail = TakeFromInput({&edges[0], &edges[1], &cleared});
	Require(clearedTail.transactions.back().frame.value == cleared.identity.frame,
			"Consumed recenter reset shortened the selected range");
	edges[0].context = range.front()->context;
	edges[1].head = edges[0].head;
	edges[1].scenarios = edges[0].scenarios;
	edges[1].context = edges[0].context;
	Require(refuses({&edges[0], &edges[2]}), "Take silently bridged missing input");
	++edges[1].epoch;
	Require(refuses({&edges[0], &edges[1]}), "Take mixed connection epochs");
	Require(refuses({&edges[0], nullptr}), "Take dereferenced evicted input");
	--edges[1].epoch;
	edges[1].complete = false;
	Require(refuses({&edges[0], &edges[1]}), "Take invented absent controller input");
	edges[1].complete = true;
	const std::vector<wire::Field> duplicateContext{
		wire::EncodeSimulationCommand(SetViewSettings{}, 1), wire::EncodeSimulationCommand(SetBodyProportions{}, 2),
		wire::EncodeSimulationCommand(SetControllerProfile{0, "left"}, 3),
		wire::EncodeSimulationCommand(SetControllerProfile{0, "left again"}, 4),
		wire::EncodeSimulationCommand(SetReferenceSpaces{}, 5)};
	edges[1].context = std::make_shared<const std::vector<std::byte>>(wire::EncodeFields(duplicateContext));
	Require(refuses({&edges[0], &edges[1]}), "Take accepted duplicate left profile with no right profile");

	auto partial = page(Frames + 1, 1, true);
	auto partialFrames = wire::DecodeFields(partial.Find(tag::Frames)->value).fields;
	auto partialFrame = wire::DecodeFields(partialFrames.front().value).fields;
	for (auto& field : partialFrame)
		if (field.tag == tag::AppliedInput)
		{
			auto input = wire::DecodeFields(field.value).fields;
			std::erase_if(input, [](const auto& item) { return item.tag == 4; });
			field.value = wire::EncodeFields(input);
		}
	partialFrames.front().value = wire::EncodeFields(partialFrame);
	for (auto& field : partial.fields)
		if (field.tag == tag::Frames)
			field.value = wire::EncodeFields(partialFrames);
	Require(feed.Accept(partial).empty(), "Partial input must remain inspectable");
	const auto* incomplete = feed.Input().Find(feed.History().At(feed.History().Size() - 1).input);
	Require(incomplete && !incomplete->complete, "Absent controller input was marked complete");

	FrameHistoryFeed older;
	older.SetScope({1, 9, 2});
	Require(older.Accept(page(1, 4, false)).empty(), "Head-only page rejected");
	std::vector<const InputSample*> headOnly;
	for (std::size_t index = 0; index < older.History().Size(); ++index)
		headOnly.push_back(older.Input().Find(older.History().At(index).input));
	bool refused = false;
	try
	{
		TakeFromInput(headOnly);
	}
	catch (const std::invalid_argument&)
	{
		refused = true;
	}
	Require(refused, "A take was cut from frames without body, gaze and trackers");
}

void InputByteBudget()
{
	using namespace simxr;
	constexpr std::size_t Budget = 128 * 1024;
	InputHistory input(64, Budget);
	const auto baseline = input.RetainedBytes();
	InputSample sample;
	sample.body = std::make_shared<const BodyControls>();
	TrackerDevices trackers;
	for (std::size_t index = 0; index < MaxTrackerDevices; ++index)
		trackers.push_back({{index + 1}, TrackerRole::Unassigned});
	sample.trackers = std::make_shared<const TrackerDevices>(trackers);
	std::uint64_t first = 0, last = 0;
	std::weak_ptr<const BodyObservation> oldest;
	for (std::size_t index = 0; index < 300; ++index)
	{
		BodyObservation observation;
		observation.requestedTime = {Duration{static_cast<std::int64_t>(index + 1)}};
		for (const auto joint : BodyModel::JointMap(observation.jointSet))
			observation.joints[static_cast<std::size_t>(joint)] = ObservedBodyJoint{IdentityPose(), 0};
		sample.bodyObservation = std::make_shared<const BodyObservation>(observation);
		last = input.Append(sample);
		if (!index)
		{
			first = last;
			oldest = sample.bodyObservation;
		}
		Require(input.Find(last) && input.RetainedBytes() <= Budget, "Dense provider history exceeds byte budget");
	}
	Require(!input.Find(first) && oldest.expired(), "Byte-evicted body storage or identity survived");
	const auto retained = input.RetainedBytes();
	const auto duplicate = input.Append(sample);
	Require(input.Find(duplicate)->bodyObservation == input.Find(last)->bodyObservation &&
				input.RetainedBytes() - retained < 128,
			"Shared provider block was charged twice");
	InputSample tooLarge;
	tooLarge.context = std::make_shared<const std::vector<std::byte>>(Budget * 2);
	const auto skipped = input.Append(std::move(tooLarge));
	Require(!input.Find(skipped) && !input.Find(last) && input.RetainedBytes() == baseline,
			"Oversize input retained memory or resolved to another frame");
	const auto recovered = input.Append(sample);
	Require(input.Find(recovered) && !input.Find(skipped), "Input history failed to recover after oversized record");
	input.Clear();
	Require(input.RetainedBytes() == baseline && !input.Find(recovered), "Clear left charged provider blocks");
}

void ClearInput()
{
	InputHistory input(2);
	InputSample sample;
	sample.body = std::make_shared<const simxr::BodyControls>();
	std::weak_ptr<const simxr::BodyControls> body = sample.body;
	const auto first = input.Append(std::move(sample));
	Require(input.Find(first) && !body.expired(), "Input history lost retained storage");
	input.Clear();
	Require(!input.Find(first) && body.expired(), "Clearing history retained its sample or shared body");
	const auto second = input.Append({});
	Require(second > first && !input.Find(first) && input.Find(second), "Clear reused a stale sample identity");
	input.Append({});
	input.Append({});
	Require(!input.Find(second), "Wrapped input ring retained an overwritten sample");
}

void ThirtyMinutes()
{
	HistoryStore history;
	for (std::uint64_t frame = 0; frame < 162000; ++frame)
		history.Append({{1, 1, frame},
						1,
						0,
						double(frame) / 90,
						frame == 81000 ? 42.0f : 11.1f,
						frame == 81000 ? HistoryDropped : 0U});
	const auto started = std::chrono::steady_clock::now();
	std::size_t records = 0, lateColumns = 0;
	for (int draw = 0; draw < 300; ++draw)
		for (std::size_t column = 0; column < 1280; ++column)
		{
			const auto bucket = history.Query(column * history.Size() / 1280, (column + 1) * history.Size() / 1280);
			records += bucket.count;
			lateColumns += (bucket.flags & HistoryDropped) != 0;
		}
	Require(records == 300 * history.Size() && lateColumns == 300, "Zoomed-out canvas lost records or drop marker");
	const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	std::cout << "162000 records, 1280 columns: " << seconds * 1000 / 300 << " ms per query pass\n";
}
} // namespace

int main()
{
	try
	{
		RingAndBuckets();
		RuntimeFeed();
		AppliedInputFeed();
		InputByteBudget();
		ClearInput();
		ThirtyMinutes();
		std::cout << "PASS history wrap, min/max/flags aggregation, identity and binary hit-testing\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
