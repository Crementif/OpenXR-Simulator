#include "headset/HeadsetSession.h"
#include "Pch.h"
#include "headset/HeadsetActions.h"
#include "headset/HeadsetBody.h"
#include "headset/HeadsetGraphics.h"
#include "headset/HeadsetImages.h"
#include "headset/HeadsetOutput.h"
#include "headset/HeadsetTestPattern.h"
#include "headset/OpenXrRuntime.h"
#include "math/Math.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <iostream>
#include <openxr/openxr_platform.h>
#include <stdexcept>
#include <thread>

namespace simxr::headset
{
HeadsetSession::HeadsetSession(OpenXrRuntime& runtime, HeadsetGraphics& graphics)
	: m_runtime(runtime), m_graphics(graphics)
{
}
HeadsetSession::~HeadsetSession()
{
	Close();
}

bool HeadsetSession::Close()
{
	m_outputState = HeadsetOutputState::Waiting;
	if (!m_session && !m_pattern && !m_output && !m_images)
		return !m_graphics.ResourcesPreserved();
	m_output.reset();
	m_images.reset();
	m_pattern.reset();
	if (!m_graphics.Drain())
	{
		m_runtime.PreserveNativeResources();
		return false;
	}
	m_body.reset();
	m_actions.reset();
	if ((m_space && XR_FAILED(xrDestroySpace(m_space))) || (m_session && XR_FAILED(xrDestroySession(m_session))))
	{
		m_graphics.PreserveNativeResources();
		m_runtime.PreserveNativeResources();
		return false;
	}
	m_space = XR_NULL_HANDLE;
	m_session = XR_NULL_HANDLE;
	return true;
}

void HeadsetSession::ApplyHaptics(const std::array<std::optional<HeadsetVibration>, 2>& commands)
{
	if (!m_running || !m_actions)
		return;
	for (std::uint32_t hand = 0; hand < commands.size(); ++hand)
		if (const auto& command = commands[hand])
		{
			if (command->amplitude == 0 || command->remaining <= 0)
				m_actions->StopHaptic(hand);
			else
				m_actions->ApplyHaptic(hand, command->amplitude, command->frequency, command->remaining);
		}
}

void HeadsetSession::SetCalibration(std::optional<XrPosef> worldFromInitial)
{
	const auto normalized = worldFromInitial ? NormalizedPose(*worldFromInitial) : std::nullopt;
	if (worldFromInitial && !normalized)
		throw std::invalid_argument("Invalid physical output calibration");
	m_worldFromInitial = normalized;
}

void HeadsetSession::Open(std::optional<HeadsetImageSource> source, bool trackers, std::optional<BodyJointSet> body)
{
	if (m_session)
		throw std::logic_error("Physical session is already open");
	m_spaceHistory = {};
	std::uint32_t count = 0;
	CheckXr(xrEnumerateEnvironmentBlendModes(m_runtime.Instance(), m_runtime.System(),
											 XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr),
			"Enumerate physical blend modes");
	std::vector<XrEnvironmentBlendMode> modes(count);
	CheckXr(xrEnumerateEnvironmentBlendModes(m_runtime.Instance(), m_runtime.System(),
											 XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, modes.data()),
			"Read physical blend modes");
	if (std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) == modes.end())
		throw std::runtime_error("The physical runtime lacks opaque stereo output");
	XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
	binding.instance = m_graphics.Instance();
	binding.physicalDevice = m_graphics.PhysicalDevice();
	binding.device = m_graphics.Device();
	binding.queueFamilyIndex = m_graphics.QueueFamily();
	XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO, &binding};
	create.systemId = m_runtime.System();
	CheckXr(xrCreateSession(m_runtime.Instance(), &create, &m_session), "Create physical OpenXR session");
	CheckXr(xrEnumerateReferenceSpaces(m_session, 0, &count, nullptr), "Enumerate physical reference spaces");
	std::vector<XrReferenceSpaceType> spaces(count);
	CheckXr(xrEnumerateReferenceSpaces(m_session, count, &count, spaces.data()), "Read physical reference spaces");
	spaces.resize(count);
	m_referenceType = XR_REFERENCE_SPACE_TYPE_MAX_ENUM;
	for (const auto candidate :
		 {XR_REFERENCE_SPACE_TYPE_STAGE, XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT, XR_REFERENCE_SPACE_TYPE_LOCAL})
		if (std::find(spaces.begin(), spaces.end(), candidate) != spaces.end())
		{
			m_referenceType = candidate;
			break;
		}
	if (m_referenceType == XR_REFERENCE_SPACE_TYPE_MAX_ENUM)
		throw std::runtime_error("Physical runtime has no supported world reference space");
	std::cout << "physical_reference_space=" << m_referenceType << '\n'
			  << "physical_floor_origin=" << (m_referenceType != XR_REFERENCE_SPACE_TYPE_LOCAL) << '\n';
	XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	reference.referenceSpaceType = m_referenceType;
	reference.poseInReferenceSpace.orientation.w = 1;
	CheckXr(xrCreateReferenceSpace(m_session, &reference, &m_space), "Create physical tracking space");
	m_actions = std::make_unique<HeadsetActions>(m_runtime);
	m_actions->Open(m_session, trackers);
	if (body)
	{
		m_body = std::make_unique<HeadsetBody>(m_runtime);
		m_body->Open(m_session, *body);
	}
	if (source)
	{
		m_output = std::make_unique<HeadsetOutput>(m_graphics);
		m_output->Open(m_runtime, m_session);
		m_images = std::make_unique<HeadsetImages>(m_graphics, std::move(*source), m_output->Device());
	}
	else
	{
		m_pattern = std::make_unique<HeadsetTestPattern>(m_graphics);
		m_pattern->Open(m_session);
	}
}

void HeadsetSession::PollEvents()
{
	for (;;)
	{
		XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
		const auto result = xrPollEvent(m_runtime.Instance(), &event);
		if (result == XR_EVENT_UNAVAILABLE)
			return;
		CheckXr(result, "Poll physical runtime events");
		if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
		{
			m_exiting = true;
			return;
		}
		if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
		{
			const auto& change = reinterpret_cast<const XrEventDataReferenceSpaceChangePending&>(event);
			if (change.session == m_session && change.referenceSpaceType == m_referenceType)
				m_spaceHistory.Schedule(change.changeTime, change.poseValid, change.poseInPreviousSpace);
			continue;
		}
		if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
			continue;
		const auto& state = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
		if (state.session != m_session)
			continue;
		std::cout << "physical_session_state=" << state.state << '\n';
		if (state.state == XR_SESSION_STATE_READY && !m_running && !m_exitRequested)
		{
			XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
			begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			CheckXr(xrBeginSession(m_session, &begin), "Begin physical session");
			m_running = true;
			m_lastPrediction = 0;
		}
		else if (state.state == XR_SESSION_STATE_STOPPING && m_running)
		{
			CheckXr(xrEndSession(m_session), "End physical session");
			m_running = false;
			m_exiting = true;
		}
		else if (state.state == XR_SESSION_STATE_EXITING || state.state == XR_SESSION_STATE_LOSS_PENDING)
			m_exiting = true;
	}
}

HeadsetImageTiming HeadsetSession::ImageTiming() const
{
	return m_images ? m_images->Timing() : HeadsetImageTiming{};
}

bool HeadsetSession::Frame(bool render, const std::function<void(const HeadsetInputSample&)>& observe)
{
	XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
	XrFrameState state{XR_TYPE_FRAME_STATE};
	const auto waitStarted = std::chrono::steady_clock::now();
	CheckXr(xrWaitFrame(m_session, &wait, &state), "Wait for physical display");
	m_timing.maximumWait =
		std::max(m_timing.maximumWait,
				 std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStarted));
	XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
	CheckXr(xrBeginFrame(m_session, &begin), "Begin physical frame");
	if (state.predictedDisplayTime <= m_lastPrediction || state.predictedDisplayPeriod <= 0)
		throw std::runtime_error("Physical runtime returned an invalid display prediction");
	m_lastPrediction = state.predictedDisplayTime;
	const auto space = m_spaceHistory.At(state.predictedDisplayTime);
	auto input = m_actions->Sample(state.predictedDisplayTime, m_space);
	input.displayPeriod = state.predictedDisplayPeriod;
	input.space = space;
	input.referenceType = m_referenceType;
	if (m_body)
		input.body = m_body->Sample(state.predictedDisplayTime, m_space, input.focused);
	constexpr auto validPose = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if ((input.head.locationFlags & validPose) == validPose)
		++m_validHeadSamples;
	for (std::size_t hand = 0; hand < input.hands.size(); ++hand)
		if ((input.hands[hand].grip.locationFlags & validPose) == validPose)
			++m_validGripSamples[hand];
	std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
	XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
	locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate.displayTime = state.predictedDisplayTime;
	locate.space = m_space;
	XrViewState viewState{XR_TYPE_VIEW_STATE};
	std::uint32_t count = 0;
	CheckXr(xrLocateViews(m_session, &locate, &viewState, views.size(), &count, views.data()),
			"Locate physical eye views");
	if (count != views.size())
		throw std::runtime_error("Physical stereo runtime returned an unexpected eye count");
	input.viewsLocated = true;
	XrSpaceLocationFlags eyeFlags = 0;
	if (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)
		eyeFlags |= XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)
		eyeFlags |= XR_SPACE_LOCATION_POSITION_VALID_BIT;
	if (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_TRACKED_BIT)
		eyeFlags |= XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
	if (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_TRACKED_BIT)
		eyeFlags |= XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	for (std::size_t eye = 0; eye < views.size(); ++eye)
	{
		input.eyes[eye].location.pose = views[eye].pose;
		input.eyes[eye].location.locationFlags = eyeFlags;
		input.eyes[eye].fov = views[eye].fov;
	}
	if (observe)
		observe(input);
	const auto required = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
	const bool eligible =
		render && state.shouldRender && count == views.size() && (viewState.viewStateFlags & required) == required;
	bool submitted = false, inspection = false;
	std::array<XrCompositionLayerQuad, 2> screens{};
	std::array<XrCompositionLayerProjectionView, 2> projection{
		{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
	if (m_output)
	{
		m_output->Poll();
		m_images->Tick();
		if (eligible && m_worldFromInitial && m_images->Fresh(std::chrono::nanoseconds(state.predictedDisplayPeriod)))
		{
			m_inspectionAnchor.reset();
			const auto worldFromCurrent = Compose(*m_worldFromInitial, space.initialFromCurrent);
			submitted = m_output->Draw(m_images->Current(), RelativePose(IdentityPose(), worldFromCurrent),
									   {views[0].fov, views[1].fov});
			if (submitted)
				projection = m_output->Projection();
		}
		else if (eligible && m_images->Current() && (input.head.locationFlags & validPose) == validPose)
		{
			if (!m_inspectionAnchor)
				m_inspectionAnchor =
					Compose(Compose(space.initialFromCurrent, input.head.pose), XrPosef{{0, 0, 0, 1}, {0, 0, -2}});
			const auto frame = m_images->Current();
			submitted = m_output->DrawScreen(frame);
			inspection = submitted;
			const auto pose = RelativePose(*m_inspectionAnchor, space.initialFromCurrent);
			const auto& description = frame->metadata.description;
			float width = 2, height = 2.f * description.height / description.width;
			if (height > 1.5f)
			{
				width *= 1.5f / height;
				height = 1.5f;
			}
			for (std::uint32_t eye = 0; eye < 2; ++eye)
			{
				screens[eye] = {XR_TYPE_COMPOSITION_LAYER_QUAD};
				screens[eye].space = m_space;
				screens[eye].eyeVisibility = eye ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
				screens[eye].subImage = {m_output->Swapchain(), {{0, 0}, m_output->Extent()}, eye};
				screens[eye].pose = pose;
				screens[eye].size = {width, height};
			}
		}
		if (!m_images->Current())
			m_inspectionAnchor.reset();
	}
	else
	{
		submitted = eligible && m_pattern->Draw();
		for (std::uint32_t eye = 0; eye < 2; ++eye)
		{
			projection[eye].pose = views[eye].pose;
			projection[eye].fov = views[eye].fov;
			projection[eye].subImage = {m_pattern->Swapchain(), {{0, 0}, m_pattern->Extent()}, eye};
		}
	}
	XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	layer.space = m_space;
	layer.viewCount = projection.size();
	layer.views = projection.data();
	const XrCompositionLayerBaseHeader* layers[] = {
		inspection ? reinterpret_cast<const XrCompositionLayerBaseHeader*>(&screens[0])
				   : reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer),
		reinterpret_cast<const XrCompositionLayerBaseHeader*>(&screens[1])};
	XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
	end.displayTime = state.predictedDisplayTime;
	end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end.layerCount = submitted ? (inspection ? 2 : 1) : 0;
	end.layers = submitted ? layers : nullptr;
	const auto endStarted = std::chrono::steady_clock::now();
	CheckXr(xrEndFrame(m_session, &end), "End physical frame");
	m_timing.maximumEndFrame =
		std::max(m_timing.maximumEndFrame,
				 std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - endStarted));
	m_timing.physicalPrediction = state.predictedDisplayTime;
	m_timing.sourceSession = m_timing.imageTick = 0;
	m_timing.sourceFrame = {};
	m_timing.sourceAge.reset();
	++m_timing.frames;
	m_timing.projected += submitted && !inspection;
	m_timing.inspected += submitted && inspection;
	m_timing.empty += !submitted;
	if (submitted && m_images && m_images->Current())
	{
		const auto& metadata = m_images->Current()->metadata;
		const auto image = metadata.imageTick;
		m_timing.sourceSession = metadata.session;
		m_timing.sourceFrame = metadata.frame;
		m_timing.imageTick = image;
		m_timing.sourceAge = m_images->Age(endStarted);
		m_timing.repeated += image && image == m_lastPresentedImage;
		m_lastPresentedImage = image;
	}
	else
		m_lastPresentedImage = 0;
	m_outputState = !m_output			  ? HeadsetOutputState::Waiting
					: !state.shouldRender ? HeadsetOutputState::Hidden
					: !submitted		  ? HeadsetOutputState::Waiting
					: inspection		  ? HeadsetOutputState::Inspection
										  : HeadsetOutputState::Live;
	return submitted;
}

void HeadsetSession::Run(std::optional<std::uint32_t> frames,
						 const std::function<void(const HeadsetInputSample&)>& observe,
						 const std::function<bool()>& stopRequested)
{
	if (!m_session || (frames && !*frames))
		throw std::logic_error("Open a physical session with a positive frame limit or continuous operation");
	std::uint64_t submitted = 0;
	bool cancelled = false;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (!m_exiting)
	{
		PollEvents();
		if (m_exiting)
			break;
		if ((frames || !m_running || m_exitRequested) && std::chrono::steady_clock::now() >= deadline)
			throw std::runtime_error("Physical session timed out waiting for presentation or shutdown");
		if (!cancelled && stopRequested && stopRequested())
			cancelled = true;
		if (cancelled && !m_running)
			break;
		if (!m_running)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			continue;
		}
		if ((cancelled || (frames && submitted >= *frames)) && !m_exitRequested)
		{
			ApplyHaptics({HeadsetVibration{}, HeadsetVibration{}});
			CheckXr(xrRequestExitSession(m_session), "Request physical session exit");
			m_exitRequested = true;
			deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			continue;
		}
		if (Frame(!m_exitRequested, observe))
			++submitted;
	}
	std::cout << (frames ? "submitted_test_frames=" : "submitted_frames=") << submitted << '\n';
	std::cout << "valid_head_samples=" << m_validHeadSamples << '\n'
			  << "valid_left_grip_samples=" << m_validGripSamples[0] << '\n'
			  << "valid_right_grip_samples=" << m_validGripSamples[1] << '\n';
	if (!cancelled && frames && submitted < *frames)
		throw std::runtime_error("Physical session ended before the requested stereo test completed");
}
} // namespace simxr::headset
