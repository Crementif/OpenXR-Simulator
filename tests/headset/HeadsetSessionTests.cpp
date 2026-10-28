#include "../TestSupport.h"
#include "Pch.h"
#include "headset/HeadsetActions.h"
#include "headset/HeadsetGraphics.h"
#include "headset/HeadsetImages.h"
#include "headset/HeadsetOutput.h"
#include "headset/HeadsetSession.h"
#include "headset/HeadsetTestPattern.h"
#include "headset/OpenXrRuntime.h"
#include "math/Math.h"

#include <iostream>

namespace simxr::headset::tests
{
struct State
{
	bool ready = false, running = false, stop = false, stopped = false;
	bool badPrediction = false, failDrain = false, preserved = false, originEvent = false;
	unsigned waits = 0, begins = 0, ends = 0, rendered = 0, draws = 0;
	unsigned destroyedSession = 0, destroyedSpace = 0;
	unsigned bodySamples = 0, destroyedBody = 0;
	bool bodyOpen = false, failBody = false;
	std::optional<BodyJointSet> bodySet;
	unsigned vibrations = 0, hapticStops = 0;
	bool stage = false, localFloor = false, live = false;
	unsigned imageTicks = 0, liveDraws = 0, screenDraws = 0;
	XrPosef outputTransform{};
	XrReferenceSpaceType createdReference = XR_REFERENCE_SPACE_TYPE_MAX_ENUM;
};
static State s_state;
static XrSession Session()
{
	return reinterpret_cast<XrSession>(std::uintptr_t{2});
}
static XrSpace Space()
{
	return reinterpret_cast<XrSpace>(std::uintptr_t{3});
}
} // namespace simxr::headset::tests

namespace simxr
{
struct VulkanBackend::State
{
};
VulkanBackend::~VulkanBackend() = default;
} // namespace simxr
namespace simxr::control
{
struct VulkanImageConsumer::State
{
};
VulkanImageConsumer::~VulkanImageConsumer() = default;
} // namespace simxr::control
namespace simxr::headset
{
HeadsetImageBridge::HeadsetImageBridge(HeadsetImageSource source) : m_source(std::move(source)) {}
HeadsetImageBridge::~HeadsetImageBridge() = default;
HeadsetImages::HeadsetImages(HeadsetGraphics& graphics, HeadsetImageSource source, std::shared_ptr<VulkanDevice>)
	: m_graphics(graphics), m_bridge(std::move(source))
{
	m_current = std::make_shared<HeadsetImageFrame>();
	m_current->metadata.imageTick = 1;
	m_current->metadata.session = 42;
	m_current->metadata.frame = {{3}, {9}};
	m_current->metadata.description.width = 800;
	m_current->metadata.description.height = 600;
}
HeadsetImages::~HeadsetImages() = default;
std::optional<std::chrono::nanoseconds> HeadsetImages::Age(std::chrono::steady_clock::time_point) const
{
	return std::chrono::milliseconds(7);
}
bool HeadsetImages::Fresh(std::chrono::nanoseconds) const
{
	return tests::s_state.waits != 5;
}
void HeadsetImages::Tick()
{
	++tests::s_state.imageTicks;
}
std::shared_ptr<const HeadsetImageFrame> HeadsetImages::Current() const
{
	return m_current;
}
HeadsetOutput::HeadsetOutput(HeadsetGraphics& graphics) : m_graphics(graphics) {}
HeadsetOutput::~HeadsetOutput() = default;
HeadsetOutputResources::~HeadsetOutputResources() = default;
void HeadsetOutput::Open(OpenXrRuntime&, XrSession)
{
	tests::s_state.live = true;
}
void HeadsetOutput::Poll() {}
bool HeadsetOutput::DrawScreen(std::shared_ptr<const HeadsetImageFrame>)
{
	++tests::s_state.screenDraws;
	return true;
}
XrSwapchain HeadsetOutput::Swapchain() const
{
	return reinterpret_cast<XrSwapchain>(4);
}
XrExtent2Di HeadsetOutput::Extent() const
{
	return {800, 600};
}
std::shared_ptr<VulkanDevice> HeadsetOutput::Device() const
{
	return {};
}
bool HeadsetOutput::Draw(std::shared_ptr<const HeadsetImageFrame> source, XrPosef transform,
						 const std::array<XrFovf, 2>&)
{
	Require(source != nullptr, "Live session omitted its private image");
	++tests::s_state.liveDraws;
	tests::s_state.outputTransform = transform;
	for (std::size_t eye = 0; eye < 2; ++eye)
	{
		m_projection[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
		m_projection[eye].pose = IdentityPose();
		m_projection[eye].pose.position.x = 42 + eye;
		m_projection[eye].fov = {-.8f, .9f, 1.f, -.5f};
	}
	return true;
}
const std::array<XrCompositionLayerProjectionView, 2>& HeadsetOutput::Projection() const
{
	return m_projection;
}
HeadsetBody::HeadsetBody(OpenXrRuntime& runtime) : m_runtime(runtime) {}
HeadsetBody::~HeadsetBody()
{
	if (tests::s_state.bodyOpen && !tests::s_state.preserved)
	{
		Require(!tests::s_state.destroyedSession, "Body must be retired before its native session");
		tests::s_state.bodyOpen = false;
		++tests::s_state.destroyedBody;
	}
}
void HeadsetBody::Open(XrSession session, BodyJointSet set)
{
	Require(session == tests::Session(), "Body belongs to selected native session");
	if (tests::s_state.failBody)
		throw std::runtime_error("Body creation failed");
	tests::s_state.bodyOpen = true;
	tests::s_state.bodySet = set;
}
PhysicalBodySample HeadsetBody::Sample(XrTime time, XrSpace space, bool locate)
{
	Require(tests::s_state.bodyOpen && space == tests::Space(), "Sample body in the shared native base space");
	++tests::s_state.bodySamples;
	PhysicalBodySample sample;
	sample.jointSet = *tests::s_state.bodySet;
	sample.requestedTime = time;
	sample.located = locate;
	return sample;
}
HeadsetActions::HeadsetActions(OpenXrRuntime& runtime) : m_runtime(runtime) {}
HeadsetActions::~HeadsetActions() = default;
HeadsetTrackers::~HeadsetTrackers() = default;
void HeadsetActions::Open(XrSession, bool) {}
bool HeadsetActions::ApplyHaptic(std::uint32_t hand, float amplitude, float frequency, XrDuration duration)
{
	Require(hand == 0 && amplitude == .5f && frequency == 80 && duration == 1000,
			"Dispatch physical vibration parameters");
	++tests::s_state.vibrations;
	return true;
}
bool HeadsetActions::StopHaptic(std::uint32_t)
{
	++tests::s_state.hapticStops;
	return true;
}
HeadsetInputSample HeadsetActions::Sample(XrTime time, XrSpace)
{
	HeadsetInputSample sample;
	sample.time = time;
	sample.focused = tests::s_state.waits != 2;
	sample.head.pose = IdentityPose();
	sample.head.locationFlags = ValidPoseFlags;
	sample.head.pose.position.x = tests::s_state.waits;
	return sample;
}
OpenXrRuntime::~OpenXrRuntime() = default;
XrInstance OpenXrRuntime::Instance() const
{
	return reinterpret_cast<XrInstance>(std::uintptr_t{1});
}
XrSystemId OpenXrRuntime::System() const
{
	return 1;
}
void OpenXrRuntime::PreserveNativeResources()
{
	tests::s_state.preserved = true;
}
void CheckXr(XrResult result, const char* operation)
{
	if (XR_FAILED(result))
		throw std::runtime_error(operation);
}
HeadsetGraphics::~HeadsetGraphics() = default;
VkInstance HeadsetGraphics::Instance() const
{
	return VK_NULL_HANDLE;
}
VkPhysicalDevice HeadsetGraphics::PhysicalDevice() const
{
	return VK_NULL_HANDLE;
}
VkDevice HeadsetGraphics::Device() const
{
	return VK_NULL_HANDLE;
}
std::uint32_t HeadsetGraphics::QueueFamily() const
{
	return 0;
}
bool HeadsetGraphics::Drain()
{
	if (tests::s_state.failDrain)
		m_preserved = true;
	return !m_preserved;
}
void HeadsetGraphics::PreserveNativeResources()
{
	m_preserved = true;
}
bool HeadsetGraphics::ResourcesPreserved() const
{
	return m_preserved;
}
HeadsetTestPattern::HeadsetTestPattern(HeadsetGraphics& graphics) : m_graphics(graphics) {}
HeadsetTestPattern::~HeadsetTestPattern() = default;
void HeadsetTestPattern::Open(XrSession) {}
bool HeadsetTestPattern::Draw()
{
	++tests::s_state.draws;
	return true;
}
XrSwapchain HeadsetTestPattern::Swapchain() const
{
	return reinterpret_cast<XrSwapchain>(std::uintptr_t{4});
}
XrExtent2Di HeadsetTestPattern::Extent() const
{
	return {256, 256};
}
} // namespace simxr::headset

extern "C"
{
	XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateEnvironmentBlendModes(XrInstance, XrSystemId, XrViewConfigurationType,
																	std::uint32_t capacity, std::uint32_t* count,
																	XrEnvironmentBlendMode* modes)
	{
		*count = 1;
		if (capacity)
			modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrCreateSession(XrInstance, const XrSessionCreateInfo*, XrSession* session)
	{
		*session = simxr::headset::tests::Session();
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrDestroySession(XrSession)
	{
		++simxr::headset::tests::s_state.destroyedSession;
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateReferenceSpaces(XrSession, std::uint32_t capacity, std::uint32_t* count,
															  XrReferenceSpaceType* spaces)
	{
		const auto& state = simxr::headset::tests::s_state;
		std::vector<XrReferenceSpaceType> available{XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL};
		if (state.localFloor)
			available.push_back(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT);
		if (state.stage)
			available.push_back(XR_REFERENCE_SPACE_TYPE_STAGE);
		*count = static_cast<std::uint32_t>(available.size());
		if (capacity)
		{
			Require(capacity >= available.size(), "Reference space capacity");
			std::copy(available.begin(), available.end(), spaces);
		}
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo* create,
														  XrSpace* space)
	{
		Require(create->poseInReferenceSpace.orientation.w == 1, "Identity base space orientation");
		simxr::headset::tests::s_state.createdReference = create->referenceSpaceType;
		*space = simxr::headset::tests::Space();
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace)
	{
		++simxr::headset::tests::s_state.destroyedSpace;
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrPollEvent(XrInstance, XrEventDataBuffer* buffer)
	{
		using namespace simxr::headset::tests;
		XrSessionState value;
		if (!s_state.ready)
		{
			s_state.ready = true;
			value = XR_SESSION_STATE_READY;
		}
		else if (!s_state.originEvent)
		{
			s_state.originEvent = true;
			*reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(buffer) = {
				XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING,
				nullptr,
				Session(),
				XR_REFERENCE_SPACE_TYPE_LOCAL,
				25'000'000,
				XR_TRUE,
				{{0, 0, 0, 1}, {2, 0, 0}}};
			return XR_SUCCESS;
		}
		else if (s_state.stop && !s_state.stopped)
		{
			s_state.stopped = true;
			value = XR_SESSION_STATE_STOPPING;
		}
		else
			return XR_EVENT_UNAVAILABLE;
		*reinterpret_cast<XrEventDataSessionStateChanged*>(buffer) = {XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED, nullptr,
																	  Session(), value, 1};
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrBeginSession(XrSession, const XrSessionBeginInfo* begin)
	{
		auto& state = simxr::headset::tests::s_state;
		Require(state.ready && !state.running, "Begin only after READY");
		Require(begin->primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, "Stereo session");
		state.running = true;
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrEndSession(XrSession)
	{
		auto& state = simxr::headset::tests::s_state;
		Require(state.running && state.stopped, "End only after STOPPING");
		state.running = false;
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrRequestExitSession(XrSession)
	{
		auto& state = simxr::headset::tests::s_state;
		Require(state.running, "Exit requested while running");
		state.stop = true;
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrWaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState* frame)
	{
		auto& state = simxr::headset::tests::s_state;
		Require(state.running && !state.stopped && state.waits == state.ends, "Wait/frame ordering");
		Require(++state.waits < 10, "Test cannot spin waiting for output");
		frame->predictedDisplayTime = state.badPrediction ? 0 : state.waits * 10000000;
		frame->predictedDisplayPeriod = 10000000;
		frame->shouldRender = state.waits != 1;
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrBeginFrame(XrSession, const XrFrameBeginInfo*)
	{
		auto& state = simxr::headset::tests::s_state;
		Require(state.waits == state.begins + 1, "Begin follows wait");
		++state.begins;
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrLocateViews(XrSession, const XrViewLocateInfo* locate, XrViewState* state,
												 std::uint32_t capacity, std::uint32_t* count, XrView* views)
	{
		using namespace simxr::headset::tests;
		Require(capacity == 2 && locate->space == Space(), "Locate in the selected base space");
		Require(locate->displayTime == s_state.waits * 10000000, "Locate at predicted time");
		*count = 2;
		state->viewStateFlags = s_state.waits == 2
									? 0
									: XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT |
										  XR_VIEW_STATE_ORIENTATION_TRACKED_BIT;
		for (unsigned eye = 0; eye < 2; ++eye)
		{
			views[eye].pose.orientation.w = 1;
			views[eye].pose.position.x = eye == 0 ? -0.03f : 0.03f;
			views[eye].fov = {-0.7f, 0.8f, 0.9f, -0.6f};
		}
		return XR_SUCCESS;
	}
	XRAPI_ATTR XrResult XRAPI_CALL xrEndFrame(XrSession, const XrFrameEndInfo* end)
	{
		auto& state = simxr::headset::tests::s_state;
		Require(state.begins == state.ends + 1, "End follows begin");
		++state.ends;
		Require(end->displayTime == state.waits * 10000000, "Submission keeps prediction");
		if (state.live && end->layerCount == 2)
		{
			for (std::size_t eye = 0; eye < 2; ++eye)
			{
				const auto& screen = *reinterpret_cast<const XrCompositionLayerQuad*>(end->layers[eye]);
				Require(screen.type == XR_TYPE_COMPOSITION_LAYER_QUAD && screen.pose.position.z == -2 &&
							screen.size.width == 2 && screen.size.height == 1.5f &&
							screen.subImage.imageArrayIndex == eye,
						"Inspection screen lost world anchor, aspect or stereo slice");
				Require(screen.pose.position.x == (state.waits == 3 ? 3 : 5),
						"Inspection screen followed later head motion");
			}
			return XR_SUCCESS;
		}
		if (state.live && end->layerCount)
		{
			const auto* layer = reinterpret_cast<const XrCompositionLayerProjection*>(end->layers[0]);
			Require(layer->views[0].pose.position.x == 42 && layer->views[0].fov.angleLeft == -.8f,
					"Session relabelled live pixels with latest physical views");
		}
		if (state.waits <= 2 || (state.live && (state.waits < 4 || state.waits >= 5)))
			Require(end->layerCount == 0, "No layer when rendering or eye validity is unavailable");
		else
		{
			Require(end->layerCount == 1, "One stereo projection layer");
			const auto& layer = *reinterpret_cast<const XrCompositionLayerProjection*>(end->layers[0]);
			Require(layer.viewCount == 2 && layer.space == simxr::headset::tests::Space(), "Stereo layer base space");
			if (!state.live)
			{
				Require(layer.views[0].pose.position.x == -0.03f && layer.views[1].pose.position.x == 0.03f,
						"Physical eye poses");
				Require(layer.views[0].fov.angleLeft == -0.7f && layer.views[1].fov.angleRight == 0.8f,
						"Physical asymmetric FOV");
				Require(layer.views[0].subImage.imageArrayIndex == 0 && layer.views[1].subImage.imageArrayIndex == 1,
						"Eye slices");
			}
			++state.rendered;
		}
		return XR_SUCCESS;
	}
}

namespace simxr::headset::tests
{
static void Run()
{
	{
		s_state = {};
		OpenXrRuntime runtime;
		HeadsetGraphics graphics;
		HeadsetSession session(runtime, graphics);
		session.Open();
		unsigned observations = 0;
		session.Run(
			2,
			[&](const HeadsetInputSample& input)
			{
				++observations;
				Require(!input.body, "Unselected body provider must not open or sample");
				Require(input.space.revision == (observations < 3 ? 1u : 2u) &&
							input.space.initialFromCurrent.position.x == (observations < 3 ? 0.f : 2.f),
						"Runtime origin event updates the sampled calibration only at the target prediction");
				if (observations == 1)
					session.ApplyHaptics({HeadsetVibration{.5f, 80, 1000}, std::nullopt});
				Require(input.time == s_state.waits * 10000000, "Input and views share the predicted time");
				Require(s_state.ends + 1 == s_state.waits && s_state.draws == (s_state.waits > 3 ? 1 : 0),
						"Publish coherent tracking before drawing and ending the physical frame");
				Require(input.eyes[0].location.pose.position.x == -0.03f &&
							input.eyes[1].location.pose.position.x == 0.03f && input.eyes[0].fov.angleLeft == -0.7f,
						"Input observation retains the physical eye geometry");
				const auto flags = s_state.waits == 2 ? 0
													  : XR_SPACE_LOCATION_POSITION_VALID_BIT |
															XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
															XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
				Require(input.eyes[0].location.locationFlags == flags && input.eyes[1].location.locationFlags == flags,
						"View validity maps explicitly without inventing positional tracking");
			});
		Require(s_state.vibrations == 1 && s_state.hapticStops == 2,
				"Dispatch output and stop both hands before session exit");
		Require(observations == 4, "Tracking is observed even when rendering or eye tracking is unavailable");
		const auto timing = session.Timing();
		Require(timing.sourceSession == 0 && timing.imageTick == 0 && !timing.sourceAge,
				"Test pattern cannot invent an application source identity or age");
		Require(timing.frames == 4 && timing.projected == 2 && timing.empty == 2 && timing.inspected == 0 &&
					timing.repeated == 0 && timing.maximumWait.count() >= 0 && timing.maximumEndFrame.count() >= 0,
				"Physical submission counters must distinguish suppressed frames from projections");
		Require(session.Close() && session.Close(), "Repeated close is safe");
		Require(s_state.waits == 4 && s_state.rendered == 2 && s_state.draws == 2,
				"Suppression does not upload/render");
		Require(s_state.stopped && !s_state.running, "Requested shutdown reaches STOPPING");
		Require(s_state.destroyedSession == 1 && s_state.destroyedSpace == 1, "Destroy each native handle once");
	}
	for (const auto set :
		 {BodyJointSet::UpperBodyFB, BodyJointSet::FullBodyMETA, BodyJointSet::WithoutArmsBD, BodyJointSet::FullBodyBD})
	{
		s_state = {};
		OpenXrRuntime runtime;
		HeadsetGraphics graphics;
		HeadsetSession session(runtime, graphics);
		session.Open({}, false, set);
		session.Run(2,
					[&](const HeadsetInputSample& input)
					{
						Require(input.body && input.body->jointSet == set && input.body->requestedTime == input.time,
								"Selected body observations travel with the predicted native input batch");
						Require(input.body->located == input.focused,
								"Unfocused session suppresses native body locate");
					});
		Require(s_state.bodySamples == 4 && session.Close() && session.Close() && s_state.destroyedBody == 1,
				"Body lifetime follows the native session with idempotent cleanup");
	}
	{
		s_state = {};
		s_state.failBody = true;
		OpenXrRuntime runtime;
		HeadsetGraphics graphics;
		HeadsetSession session(runtime, graphics);
		bool rejected = false;
		try
		{
			session.Open({}, false, BodyJointSet::UpperBodyFB);
		}
		catch (const std::runtime_error&)
		{
			rejected = true;
		}
		Require(rejected && session.Close() && s_state.destroyedSession == 1 && !s_state.bodyOpen,
				"Failed body creation retains clean native session shutdown");
	}
	for (const unsigned stopAfter : {0u, 1u})
	{
		s_state = {};
		OpenXrRuntime runtime;
		HeadsetGraphics graphics;
		HeadsetSession session(runtime, graphics);
		session.Open();
		unsigned observations = 0;
		session.Run(
			1000,
			[&](const HeadsetInputSample&)
			{
				++observations;
			},
			[&]
			{
				return observations == stopAfter;
			});
		Require(observations == stopAfter && s_state.waits == s_state.ends && s_state.stopped && !s_state.running,
				"Lost target cancels before the test limit without abandoning an open frame");
		Require(s_state.hapticStops == 2 && session.Close(),
				"Cancellation stops feedback and retires the physical session");
	}
	{
		s_state = {};
		OpenXrRuntime runtime;
		HeadsetGraphics graphics;
		HeadsetSession session(runtime, graphics);
		session.Open();
		unsigned observations = 0;
		session.Run(
			std::nullopt,
			[&](const HeadsetInputSample&)
			{
				++observations;
			},
			[&]
			{
				return observations >= 7;
			});
		Require(observations == 7 && s_state.stopped && s_state.waits == s_state.ends && session.Close(),
				"Continuous session keeps presenting until its owner requests stop");
	}
	for (const bool stage : {false, true})
	{
		s_state = {};
		s_state.stage = stage;
		OpenXrRuntime runtime;
		HeadsetGraphics graphics;
		HeadsetSession session(runtime, graphics);
		session.Open(HeadsetImageSource{"test", 1, 1});
		unsigned observations = 0;
		session.Run(
			std::nullopt,
			[&](const HeadsetInputSample&)
			{
				++observations;
				const auto previous = session.OutputState();
				if (observations == 2)
					Require(previous == HeadsetOutputState::Hidden, "Render suppression reports hidden output");
				if (observations == 4 || observations >= 6)
					Require(previous == HeadsetOutputState::Inspection,
							"Submitted inspection screen is reported separately");
				if (observations == 5)
					Require(previous == HeadsetOutputState::Live,
							"Only successful immersive submission reports live output");
				if (observations == 4)
				{
					auto calibration = IdentityPose();
					calibration.position = {3, 0, -2};
					session.SetCalibration(calibration);
				}
				if (observations == 6)
					session.SetCalibration({});
			},
			[&]
			{
				return observations >= 7;
			});
		const auto timing = session.Timing();
		Require(timing.sourceSession == 42 && timing.sourceFrame == FrameKey{{3}, {9}} && timing.imageTick == 1 &&
					timing.sourceAge == std::chrono::milliseconds(7) && timing.physicalPrediction > 0,
				"Physical submission timing lost its application correlation or source age");
		Require(timing.frames == 7 && timing.projected == 1 && timing.inspected == 4 && timing.empty == 2 &&
					timing.repeated == 4,
				"Repeated image count must follow successful projections and inspection submissions only");
		Require(s_state.imageTicks == 7 && s_state.liveDraws == 1 && s_state.screenDraws == 4 && s_state.draws == 0,
				"Live output failed calibration gating or used diagnostic rendering");
		Require(s_state.outputTransform.position.x == (stage ? -3 : -5) && s_state.outputTransform.position.z == 2,
				"Live output did not invert calibration and the time-effective physical origin");
		Require(session.Close() && session.OutputState() == HeadsetOutputState::Waiting,
				"Close resets live output state");
	}
	for (bool stage : {false, true})
	{
		s_state = {};
		s_state.stage = stage;
		s_state.localFloor = true;
		OpenXrRuntime runtime;
		HeadsetGraphics graphics;
		HeadsetSession session(runtime, graphics);
		session.Open();
		const auto expected = stage ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
		Require(s_state.createdReference == expected, "Prefer floor-based origins, with Stage before Local Floor");
		session.Run(1,
					[&](const HeadsetInputSample& input)
					{
						Require(input.referenceType == expected && input.space.revision == 1,
								"Sample identifies selected origin and ignores unrelated LOCAL recenter events");
					});
	}

	{
		s_state = {};
		OpenXrRuntime runtime;
		HeadsetGraphics graphics;
		HeadsetSession session(runtime, graphics);
		session.Open({}, false, BodyJointSet::UpperBodyFB);
		s_state.failDrain = true;
		Require(!session.Close(), "Uncertain GPU completion fails shutdown");
		Require(s_state.preserved && !s_state.destroyedSession && !s_state.destroyedSpace && !s_state.destroyedBody,
				"Preserve dependent native owners");
	}
	{
		s_state = {};
		OpenXrRuntime runtime;
		HeadsetGraphics graphics;
		HeadsetSession session(runtime, graphics);
		session.Open();
		s_state.badPrediction = true;
		bool rejected = false;
		try
		{
			session.Run(1);
		}
		catch (const std::runtime_error&)
		{
			rejected = true;
		}
		Require(rejected && s_state.draws == 0, "Invalid physical prediction cannot render");
	}
}
} // namespace simxr::headset::tests

int main()
{
	try
	{
		simxr::headset::tests::Run();
		std::cout << "Headset session coordinator checks passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
