#include "../TestSupport.h"
#include "graphics/FrameSubmission.h"
#include "runtime/Instance.h"
#include "runtime/Session.h"

#include <iostream>

namespace simxr::test
{
class CompleteOutput final : public FrameOutput
{
public:
	CompletionStatus PollCompletion() const noexcept override
	{
		return CompletionStatus::Complete;
	}
};

class SubmissionBackend final : public FrameSubmissionBackend
{
public:
	FramePool live{1}, history{1};
	XrResult result = XR_ERROR_LIMIT_REACHED;
	bool submitted = false;

	FramePool LiveReservations() const override
	{
		return live;
	}

	FramePool HistoryReservations() const override
	{
		return history;
	}

	XrResult Prepare(FrameKey, const SessionComposition&, std::shared_ptr<FrameReservation>,
					 std::span<const FrameCaptureRequest>, PreparedFrameSubmission& output) override
	{
		output.producer = output.output = std::make_shared<CompleteOutput>();
		return result;
	}

	XrResult Submit() override
	{
		submitted = true;
		return XR_SUCCESS;
	}

	bool Submitted() const noexcept override
	{
		return submitted;
	}
};

static void CapacityRecovery(bool reserveExhausted, bool exact, XrResult pressure = XR_ERROR_LIMIT_REACHED)
{
	Instance instance(reinterpret_cast<XrInstance>(902), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(903), instance);
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.ConfigureCapturePool(FramePool{4}) == XR_SUCCESS && session.Ready(timing.epoch) == XR_SUCCESS &&
				session.Begin(begin, timing) == XR_SUCCESS,
			"Start submission regression session");
	XrSpace space{};
	Require(session.Spaces().CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, {{0, 0, 0, 1}, {}}, space) ==
				XR_SUCCESS,
			"Create quad space");
	if (exact)
	{
		session.Scheduler().Pause();
		Require(session.Display().Pause(), "Pause exact capture");
		Require(session.ReserveCapture().result == SnapshotAdmission::Accepted, "Reserve exact capture");
		Require(session.Scheduler().GrantPermits() == XR_SUCCESS, "Permit exact capture");
	}
	SubmissionBackend backend;
	backend.result = pressure;
	auto blocker = reserveExhausted ? backend.live.Reserve() : nullptr;
	FramePrediction frame;
	Require(session.WaitFrame(frame) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS, "Begin frame");
	XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
	quad.space = space;
	quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	quad.pose = {{0, 0, 0, 1}, {0, 0, -2}};
	quad.size = {1, 1};
	quad.subImage.swapchain = reinterpret_cast<XrSwapchain>(904);
	quad.subImage.imageRect.extent = {32, 32};
	const auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
	XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO,			 nullptr, frame.displayTime.value.count(),
					   XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		  &layer};
	auto describe = [](XrSwapchain) -> std::optional<SwapchainDescription> {
		return SwapchainDescription{32, 32, 1, true};
	};
	const auto applicationTiming = GpuDuration::Create();
	{
		FrameSubmission submission(session, backend, applicationTiming);
		Require(submission.Execute(end, describe) ==
					(exact || pressure != XR_ERROR_LIMIT_REACHED ? pressure : XR_SUCCESS),
				"Capacity pressure violated live/exact submission contract");
	}
	Require(!backend.submitted, "Rejected preparation submitted native work");
	if (exact || pressure != XR_ERROR_LIMIT_REACHED)
	{
		Require(session.Scheduler().Snapshot().begun.has_value(), "Preparation failure consumed the begun frame");
		if (exact)
		{
			backend.result = XR_SUCCESS;
			FrameSubmission retry(session, backend);
			Require(retry.Execute(end, describe) == XR_SUCCESS && backend.submitted,
					"Exact capture did not recover on the same frame");
		}
		return;
	}
	const FrameKey key{session.Snapshot().generation, frame.id};
	const auto record = session.History().Find(key);
	applicationTiming.Store(123);
	Require(record->submission->gpuTiming && record->submission->gpuTiming->applicationNanoseconds.Load() == 123,
			"Dropped composition lost delayed application GPU timing");
	Require(record->submission->dropReason == FrameDropReason::NativeCapacity &&
				!session.PreviewHistory().Find(key).output,
			"Live drop lost its reason or fabricated preview pixels");
	blocker.reset();
	backend.result = XR_SUCCESS;
	Require(session.WaitFrame(frame) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS,
			"Frame did not recover after pressure");
	end.displayTime = frame.displayTime.value.count();
	const auto recoveredTiming = GpuDuration::Create();
	FrameSubmission recovery(session, backend, recoveredTiming);
	Require(recovery.Execute(end, describe) == XR_SUCCESS && backend.submitted, "Recovered frame did not submit");
	recoveredTiming.Store(456);
	Require(session.History()
					.Find({session.Snapshot().generation, frame.id})
					->submission->gpuTiming->applicationNanoseconds.Load() == 456,
			"Submitted frame lost delayed application GPU timing");
	std::vector<std::shared_ptr<FrameReservation>> historyReaders;
	while (auto held = session.PreviewHistory().Budget().Reserve())
		historyReaders.push_back(std::move(held));
	Require(session.WaitFrame(frame) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS,
			"Begin history-pressure frame");
	end.displayTime = frame.displayTime.value.count();
	FrameSubmission historyPressure(session, backend);
	const auto before = std::chrono::steady_clock::now();
	Require(historyPressure.Execute(end, describe) == XR_SUCCESS &&
				std::chrono::steady_clock::now() - before < std::chrono::milliseconds(250),
			"Automatic history pressure blocked application submission");
}
} // namespace simxr::test

int main()
{
	try
	{
		simxr::test::CapacityRecovery(true, false);
		simxr::test::CapacityRecovery(false, false);
		simxr::test::CapacityRecovery(false, true);
		simxr::test::CapacityRecovery(false, false, XR_ERROR_SESSION_LOST);
		simxr::test::CapacityRecovery(false, false, XR_ERROR_OUT_OF_MEMORY);
		std::cout << "PASS live capacity drops, exact capture failure and recovery\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
