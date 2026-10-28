#include "Pch.h"
#include "headset/HeadsetGraphics.h"
#include "headset/HeadsetImageProbe.h"
#include "headset/HeadsetImages.h"
#include "headset/HeadsetInputBridge.h"
#include "headset/HeadsetSession.h"
#include "headset/HostControl.h"
#include "headset/OpenXrRuntime.h"
#include "headset/RuntimeClock.h"
#include "headset/RuntimeSelection.h"
#include "math/Math.h"
#if defined(__linux__)
#include "platform/linux/InstallUse.h"
#elif defined(_WIN32)
#include "platform/windows/InstallUse.h"
#endif

#include <iostream>

namespace simxr::headset
{
static void ReportTiming(const HeadsetSession& session, const HeadsetInputStatus& input)
{
	const auto frame = session.Timing();
	const auto images = session.ImageTiming();
	const auto clock = ReadNativeClockSample();
	std::ostringstream report;
	report << "simxr_timing_clock=" << static_cast<unsigned>(clock.domain) << ',' << clock.nanoseconds << ','
		   << clock.resolutionNanoseconds << '\n'
		   << "simxr_timing_frames=" << frame.frames << ',' << frame.projected << ',' << frame.inspected << ','
		   << frame.empty << ',' << frame.repeated << '\n'
		   << "simxr_timing_runtime_max_ns=" << frame.maximumWait.count() << ',' << frame.maximumEndFrame.count()
		   << '\n'
		   << "simxr_timing_input=" << input.queuedSamples << ',' << input.dequeuedSamples << ',' << input.sent << ','
		   << input.rejectedSamples << ',' << input.discardedSamples << ',' << input.queueDepth << ','
		   << input.queueHighWater << '\n'
		   << "simxr_timing_input_max_ns=" << input.maximumQueueDelay.count() << ',' << input.maximumForwardAge.count()
		   << '\n'
		   << "simxr_timing_images=" << images.requests << ',' << images.copies << ',' << images.unchanged << ','
		   << images.clears << ',' << images.capacityWaits << ',' << images.retained << ',' << images.maximumRetained
		   << '\n'
		   << "simxr_timing_image_max_ns=" << images.maximumDelivery.count() << ','
		   << images.maximumCopyObservation.count() << '\n';
	report << "simxr_timing_source=" << frame.sourceSession << ',' << frame.sourceFrame.generation.value << ','
		   << frame.sourceFrame.frame.value << ',' << frame.imageTick << '\n'
		   << "simxr_timing_prediction_age_ns=" << frame.physicalPrediction << ','
		   << (frame.sourceAge ? frame.sourceAge->count() : -1) << '\n';
	std::cout << report.str();
}

static int Run(const std::vector<std::filesystem::path>& arguments)
{
	std::filesystem::path manifest;
	bool selected = false;
	bool validation = false;
	bool parentControl = false;
	bool takeOver = false, forwardTrackers = false;
	std::optional<BodyJointSet> forwardBody;
	bool parentStopped = false;
	bool continuous = false, probe = false, gameOutput = false, discover = false;
	std::uint32_t testFrames = 0;
	std::optional<HeadsetImageSource> imageSource;
	std::optional<HeadsetInputTarget> inputTarget;
	std::optional<HeadsetAlignment> alignment;
	std::optional<float> localEyeHeight;
	for (std::size_t index = 1; index < arguments.size(); ++index)
	{
		if (arguments[index] == "--help")
		{
			std::cout
				<< "simxr_headset_host [--system | --runtime MANIFEST] [--probe | --test-frames COUNT | --run | "
				   "--discover] "
				   "[--validation]\n"
				   "Optional: --game-output (live stereo output with physical FOV mapping)\n"
				   "Optional: --forward-trackers (include HTCX tracker input)\n"
				   "Optional: --forward-body fb-upper|meta-full|bd-without-arms|bd-full\n"
				   "Optional: --take-over (replace selected input owners)\n"
				   "Optional: --parent-control (stdin pipe; any byte or closure requests stop)\n"
				   "Optional: --copy-image ENDPOINT SESSION GENERATION CAPTURE (existing retained capture)\n"
				   "Optional: --copy-live-image ENDPOINT SESSION GENERATION (current full-resolution output)\n"
				   "Optional: --forward-input ENDPOINT SESSION GENERATION (identity calibration by "
				   "default)\n"
				   "Optional: --align-player X Z YAW FLOOR_Y [--local-eye-height METERS] (yaw in radians)\n"
				   "Probe checks capabilities. Without --game-output, test frames and run display a labelled "
				   "stereo pattern.\n"
				   "Run requires --parent-control and --forward-input and continues until disconnected.\n"
				   "Discover requires --parent-control; reports eye sizes without graphics or input forwarding.\n";
			return 0;
		}
		const auto number = [&]()
		{
			if (++index >= arguments.size())
				throw std::invalid_argument("Missing alignment number");
			const auto text = arguments[index].string();
			float value = 0;
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
			if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value))
				throw std::invalid_argument("Alignment values must be finite numbers");
			return value;
		};
		if (arguments[index] == "--game-output" && !gameOutput)
		{
			gameOutput = true;
			continue;
		}
		if (arguments[index] == "--forward-trackers" && !forwardTrackers)
		{
			forwardTrackers = true;
			continue;
		}
		if (arguments[index] == "--forward-body" && !forwardBody)
		{
			if (++index >= arguments.size())
				throw std::invalid_argument("--forward-body requires a provider joint set");
			const auto name = arguments[index].string();
			const auto found = std::find(BodyJointSetNames.begin(), BodyJointSetNames.end(), name);
			if (found == BodyJointSetNames.end())
				throw std::invalid_argument(
					"Unknown body joint set; use fb-upper, meta-full, bd-without-arms or bd-full");
			forwardBody = static_cast<BodyJointSet>(found - BodyJointSetNames.begin());
			continue;
		}
		if (arguments[index] == "--take-over" && !takeOver)
		{
			takeOver = true;
			continue;
		}
		if (arguments[index] == "--parent-control" && !parentControl)
		{
			parentControl = true;
			continue;
		}
		if (arguments[index] == "--align-player" && !alignment)
		{
			HeadsetAlignment value;
			value.worldHead.position.x = number();
			value.worldHead.position.z = number();
			value.worldHead.orientation = EulerAngles(number(), 0, 0);
			value.worldFloor = number();
			alignment = value;
			continue;
		}
		if (arguments[index] == "--local-eye-height" && !localEyeHeight)
		{
			localEyeHeight = number();
			if (*localEyeHeight <= 0)
				throw std::invalid_argument("Local eye height must be positive");
			continue;
		}
		if (arguments[index] == "--forward-input" && !inputTarget)
		{
			if (index + 3 >= arguments.size())
				throw std::invalid_argument("Input forwarding requires ENDPOINT SESSION GENERATION");
			HeadsetInputTarget target;
			const auto endpoint = arguments[++index].u8string();
			target.endpoint.assign(endpoint.begin(), endpoint.end());
			for (auto* value : {&target.session, &target.generation.value})
			{
				const auto text = arguments[++index].string();
				const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *value);
				if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !*value)
					throw std::invalid_argument("Input forwarding identities must be positive integers");
			}
			if (target.endpoint.empty())
				throw std::invalid_argument("Input forwarding endpoint is empty");
			target.worldFromPhysical = IdentityPose();
			inputTarget = std::move(target);
			continue;
		}
		if ((arguments[index] == "--copy-image" || arguments[index] == "--copy-live-image") && !imageSource)
		{
			const bool liveImage = arguments[index] == "--copy-live-image";
			if (index + (liveImage ? 3 : 4) >= arguments.size())
				throw std::invalid_argument(
					"Image copy requires ENDPOINT SESSION GENERATION and a CAPTURE for retained images");
			HeadsetImageSource source;
			const auto endpoint = arguments[++index].u8string();
			source.endpoint.assign(endpoint.begin(), endpoint.end());
			for (auto* value : {&source.session, &source.generation, &source.capture})
			{
				if (liveImage && value == &source.capture)
					break;
				const auto text = arguments[++index].string();
				const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *value);
				if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !*value)
					throw std::invalid_argument("Image copy identities must be positive integers");
			}
			if (source.endpoint.empty())
				throw std::invalid_argument("Image copy endpoint is empty");
			imageSource = std::move(source);
			continue;
		}
		if (arguments[index] == "--probe" && !probe)
		{
			probe = true;
			continue;
		}
		if (arguments[index] == "--discover" && !discover)
		{
			discover = true;
			continue;
		}
		if (arguments[index] == "--run" && !continuous)
		{
			continuous = true;
			continue;
		}
		if (arguments[index] == "--validation")
		{
			validation = true;
			continue;
		}
		if (arguments[index] == "--test-frames" && !testFrames && index + 1 < arguments.size())
		{
			const auto text = arguments[++index].string();
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), testFrames);
			if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !testFrames ||
				testFrames > 10000)
				throw std::invalid_argument("Test frame count must be between 1 and 10000");
			continue;
		}
		if (arguments[index] == "--system" && !selected)
			selected = true;
		else if (arguments[index] == "--runtime" && !selected && index + 1 < arguments.size())
		{
			selected = true;
			manifest = ValidateRuntimeManifest(arguments[++index]);
		}
		else
			throw std::invalid_argument("Expected --system, --runtime MANIFEST, --probe or --help");
	}
	if (discover && (!parentControl || continuous || probe || testFrames || inputTarget || imageSource || gameOutput ||
					 alignment || localEyeHeight || takeOver || forwardTrackers || forwardBody))
		throw std::invalid_argument("Discovery requires parent control and cannot render or forward input");
	if ((continuous && (probe || testFrames)) || (probe && testFrames))
		throw std::invalid_argument("Choose only one of --run, --test-frames or --probe");
	if (continuous && (!parentControl || !inputTarget))
		throw std::invalid_argument("Continuous run requires --parent-control and --forward-input");
	if (inputTarget && !testFrames && !continuous)
		throw std::invalid_argument("Input forwarding requires --test-frames or --run");
	if (gameOutput && (!inputTarget || (!continuous && !testFrames) || imageSource))
		throw std::invalid_argument(
			"Game output requires a running input target and cannot be combined with copy diagnostics");
	if (forwardBody && !inputTarget)
		throw std::invalid_argument("--forward-body requires --forward-input");
	if (forwardTrackers && !inputTarget)
		throw std::invalid_argument("--forward-trackers requires --forward-input");
	if (takeOver && !inputTarget)
		throw std::invalid_argument("Takeover requires an explicit input target");
	if (inputTarget)
		inputTarget->takeOver = takeOver;
	if ((alignment && !inputTarget) || (localEyeHeight && !alignment))
		throw std::invalid_argument(
			"Player alignment requires input forwarding; local height requires player alignment");
	if (alignment)
	{
		alignment->localEyeHeight = localEyeHeight;
		inputTarget->alignment = alignment;
	}
	if (parentControl)
	{
		if (!testFrames && !continuous && !discover)
			throw std::invalid_argument("Parent control requires --test-frames, --run or --discover");
		std::cout << std::unitbuf;
		ValidateHostControlPipe();
		if (HostStopRequested())
			return 0;
	}
	PrepareHostEnvironment(manifest);
	OpenXrRuntime runtime;
	runtime.Open();
	const auto& properties = runtime.Properties();
	std::cout << "runtime=" << properties.runtimeName << '\n'
			  << "version=" << XR_VERSION_MAJOR(properties.runtimeVersion) << '.'
			  << XR_VERSION_MINOR(properties.runtimeVersion) << '.' << XR_VERSION_PATCH(properties.runtimeVersion)
			  << '\n';
	const auto utf8 = manifest.u8string();
	std::cout << "selection=" << (manifest.empty() ? "system" : std::string(utf8.begin(), utf8.end())) << '\n';
	for (std::size_t eye = 0; eye < runtime.Views().size(); ++eye)
		std::cout << "eye=" << eye << " recommended=" << runtime.Views()[eye].recommendedImageRectWidth << 'x'
				  << runtime.Views()[eye].recommendedImageRectHeight << '\n';
	if (parentControl && runtime.Views().size() == 2)
		std::cout << "simxr_host_eye_sizes=" << runtime.Views()[0].recommendedImageRectWidth << ','
				  << runtime.Views()[0].recommendedImageRectHeight << ','
				  << runtime.Views()[1].recommendedImageRectWidth << ','
				  << runtime.Views()[1].recommendedImageRectHeight << '\n';
	if (parentControl)
		std::cout << "simxr_host_tracker_support="
				  << (runtime.Supports(XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME) ? "1" : "0") << '\n';
	const auto& body = runtime.BodyCapabilities();
	std::cout << "body_support_fb=" << body.fb << '\n'
			  << "body_support_meta_full_body=" << body.fullBodyMETA << '\n'
			  << "body_support_bd=" << body.bd << '\n'
			  << "body_forwarding=" << (forwardBody ? BodyJointSetNames[static_cast<std::size_t>(*forwardBody)] : "off")
			  << '\n'
			  << std::flush;
	if (discover)
	{
		while (!HostStopRequested())
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		return 0;
	}
	for (const auto& extension : runtime.Extensions())
		std::cout << "available_extension=" << extension.extensionName << '\n';
	for (const auto& layer : runtime.Layers())
		std::cout << "available_layer=" << layer.layerName << '\n';
	std::cout << "clock_roundtrip_error_ns=" << VerifyRuntimeClock(runtime) << '\n';
	HeadsetGraphics graphics;
	graphics.Open(runtime, validation);
	std::cout << "gpu=" << graphics.Properties().deviceName << '\n';
	std::cout << "gpu_uuid=";
	constexpr char digits[] = "0123456789abcdef";
	for (auto value : graphics.Identity().deviceUUID)
		std::cout << digits[value >> 4] << digits[value & 15];
	std::cout << '\n'
			  << "vulkan_api=" << VK_VERSION_MAJOR(graphics.ApiVersion()) << '.'
			  << VK_VERSION_MINOR(graphics.ApiVersion()) << '\n';
	if (imageSource)
		VerifyHeadsetImageCopy(graphics, *imageSource);
	if (testFrames || continuous)
	{
		auto restartWindow = std::chrono::steady_clock::now();
		unsigned restarts = 0;
		for (;;)
		{
			bool rebuild = false;
			HeadsetSession session(runtime, graphics);
			std::optional<HeadsetImageSource> liveSource;
			if (gameOutput)
				liveSource =
					HeadsetImageSource{inputTarget->endpoint, inputTarget->session, inputTarget->generation.value};
			session.Open(std::move(liveSource), forwardTrackers, forwardBody);
			std::unique_ptr<HeadsetInputBridge> bridge;
			if (inputTarget)
			{
				inputTarget->displayTiming = gameOutput;
				inputTarget->trackers = forwardTrackers;
				inputTarget->body = forwardBody.has_value();
				if (forwardTrackers && !runtime.Supports(XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME))
					throw std::runtime_error(
						"Selected runtime does not expose HTCX trackers; disable Forward trackers or "
						"select a compatible runtime");
				bridge = std::make_unique<HeadsetInputBridge>(*inputTarget);
			}
			std::string inputState, outputState;
			auto nextInputStatus = std::chrono::steady_clock::time_point{};
			auto nextTiming = std::chrono::steady_clock::now();
			session.Run(
				continuous ? std::nullopt : std::optional<std::uint32_t>{testFrames},
				[&](const HeadsetInputSample& input)
				{
					if (!bridge)
						return;
					if (rebuild)
						return;
					if (continuous && input.trackers &&
						std::any_of(input.trackers->begin(), input.trackers->end(),
									[](const PhysicalTrackerSample& tracker)
									{
										return tracker.needsBinding;
									}))
					{
						const auto previous = bridge->Status();
						if (previous.worldFromInitial)
						{
							if ((input.head.locationFlags & ValidPoseFlags) != ValidPoseFlags)
								return;
							inputTarget->alignment = HeadsetRestartAlignment(
								input, *previous.worldFromInitial,
								inputTarget->alignment ? inputTarget->alignment->worldFloor : 0.f);
						}
						if (input.space.revision >
							std::numeric_limits<std::uint64_t>::max() - inputTarget->spaceRevision)
							throw std::overflow_error("Headset calibration revisions exhausted");
						inputTarget->spaceRevision += input.space.revision;
						rebuild = true;
						std::cout << "simxr_host_input=suspended\nsimxr_host_output=waiting\n"
								  << "Rebuilding headset session for newly connected tracker bindings\n"
								  << std::flush;
						return;
					}
					const auto times = RuntimeInputTimes(runtime, input);
					const auto observed = ReadNativeClockSample();
					if (times.predicted >= observed.nanoseconds)
						bridge->Publish(input, observed.domain, observed.nanoseconds, times.predicted,
										times.bodyReturned);
					session.ApplyHaptics(bridge->ConsumeHaptics(input.focused && input.controlsReady));
					const auto inputStatus = bridge->Status();
					const auto timingNow = std::chrono::steady_clock::now();
					if (timingNow >= nextTiming)
					{
						ReportTiming(session, inputStatus);
						nextTiming = timingNow + std::chrono::seconds(1);
					}
					session.SetCalibration(inputStatus.phase == HeadsetInputPhase::Ready ? inputStatus.worldFromInitial
																						 : std::nullopt);
					if (parentControl)
					{
						const auto status = bridge->Status();
						const std::string state = status.phase == HeadsetInputPhase::Suspended ? "suspended"
												  : status.phase != HeadsetInputPhase::Ready || !status.sent ? "waiting"
												  : input.focused && input.controlsReady ? "ready"
																						 : "suspended";
						const auto mode = session.OutputState();
						const std::string output = mode == HeadsetOutputState::Live			? "live"
												   : mode == HeadsetOutputState::Inspection ? "inspection"
												   : mode == HeadsetOutputState::Hidden		? "hidden"
																							: "waiting";
						const auto now = std::chrono::steady_clock::now();
						if (state != inputState || output != outputState || now >= nextInputStatus)
						{
							std::cout << "simxr_host_input=" << state << '\n' << "simxr_host_output=" << output << '\n';
							if (forwardTrackers && input.trackers)
								std::cout << "simxr_host_tracker_count=" << input.trackers->size() << '\n';
							inputState = state;
							outputState = output;
							nextInputStatus = now + std::chrono::milliseconds(100);
						}
					}
				},
				[&]
				{
					if (parentControl && HostStopRequested())
					{
						parentStopped = true;
						return true;
					}
					if (rebuild)
						return true;
					if (!bridge)
						return false;
					const auto phase = bridge->Status().phase;
					return phase == HeadsetInputPhase::Failed || phase == HeadsetInputPhase::Stopped;
				});
			if (bridge)
			{
				bridge->Stop();
				const auto status = bridge->Status();
				std::cout << "forwarded_input_samples=" << status.sent << '\n'
						  << "accepted_display_predictions=" << status.predictions << '\n';
				if (status.phase == HeadsetInputPhase::Failed || (!status.sent && !parentStopped && !rebuild))
					throw std::runtime_error(status.error.empty() ? "No native input samples were forwarded"
																  : status.error);
			}
			ReportTiming(session, bridge ? bridge->Status() : HeadsetInputStatus{});
			if (!session.Close())
				throw std::runtime_error("Physical session shutdown could not establish native resource retirement");
			if (!rebuild || parentStopped)
				break;
			const auto now = std::chrono::steady_clock::now();
			if (now - restartWindow >= std::chrono::seconds(10))
			{
				restartWindow = now;
				restarts = 0;
			}
			if (++restarts > 3)
				throw std::runtime_error(
					"Tracker bindings changed repeatedly; stabilize device connections and reconnect");
			inputTarget->takeOver = false;
			inputTarget->requireNeutral = true;
		}
	}
	if (!graphics.Close())
		throw std::runtime_error("Headset GPU shutdown could not establish completion");
	if (validation)
	{
		std::cout << "validation_errors=" << graphics.ValidationErrors() << '\n';
		if (graphics.ValidationErrors())
			throw std::runtime_error("Headset Vulkan validation reported errors");
	}
	else
		std::cout << "validation=disabled\n";
	return 0;
}
} // namespace simxr::headset

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
#if defined(__linux__)
	if (!simxr::InstallationReady())
	{
		std::cerr << "OpenXR Simulator installation is being updated or is no longer available. Try again.\n";
		return 1;
	}
#elif defined(_WIN32)
	simxr::windows::InstallUse installUse;
	if (!installUse.Ready())
	{
		std::cerr << "OpenXR Simulator installation is being updated or removed. Try again.\n";
		return 1;
	}
#endif
	try
	{
		std::vector<std::filesystem::path> arguments;
		for (int index = 0; index < argc; ++index)
			arguments.emplace_back(argv[index]);
		return simxr::headset::Run(arguments);
	}
	catch (const std::exception& error)
	{
		std::cerr << "Headset host: " << error.what() << '\n';
		return 1;
	}
}
