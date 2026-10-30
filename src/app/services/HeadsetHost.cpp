#include "app/services/HeadsetHost.h"
#include "app/UiPch.h"
#include "app/services/SimulationEdits.h"
#include "app/services/Utf8Path.h"
#include "math/Math.h"

namespace simxr::app
{
HeadsetHostAlignment HeadsetAlignmentFor(const ObservedSetup& observed)
{
	const auto& head = observed.values.tracking.head;
	const auto pose = NormalizedPose(head.pose);
	if (!pose || (head.flags & ValidPoseFlags) != ValidPoseFlags)
		throw std::invalid_argument("Headset alignment requires a valid simulated head pose");
	const auto forward = Rotate(pose->orientation, {0, 0, -1});
	if (std::hypot(forward.x, forward.z) < .0001f)
		throw std::invalid_argument("Look horizontally before connecting the headset");
	const auto& root = observed.values.tracking.body.rootPose;
	const float floor = root ? root->position.y : 0;
	if (!std::isfinite(floor))
		throw std::invalid_argument("Headset alignment requires a finite player floor height");
	return {pose->position.x, pose->position.z, std::atan2(-forward.x, -forward.z), floor};
}

static std::string NumberText(float value)
{
	std::array<char, 64> buffer;
	const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
	if (result.ec != std::errc{})
		throw std::invalid_argument("Cannot encode headset alignment");
	return {buffer.data(), result.ptr};
}

static std::optional<std::array<HeadsetEyeSize, 2>> ParseEyeSizes(std::string_view line)
{
	constexpr std::string_view prefix = "simxr_host_eye_sizes=";
	if (!line.starts_with(prefix))
		return {};
	line.remove_prefix(prefix.size());
	std::array<std::uint32_t, 4> values{};
	for (std::size_t index = 0; index < values.size(); ++index)
	{
		const auto end = line.find(',');
		const auto value = line.substr(0, end);
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), values[index]);
		if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !values[index] ||
			(end == std::string_view::npos) != (index == values.size() - 1))
			return {};
		if (end != std::string_view::npos)
			line.remove_prefix(end + 1);
	}
	return std::array<HeadsetEyeSize, 2>{{{values[0], values[1]}, {values[2], values[3]}}};
}

static std::optional<std::uint32_t> ParseTrackerCount(std::string_view line)
{
	constexpr std::string_view prefix = "simxr_host_tracker_count=";
	if (!line.starts_with(prefix))
		return {};
	line.remove_prefix(prefix.size());
	std::uint32_t count = 0;
	const auto parsed = std::from_chars(line.data(), line.data() + line.size(), count);
	if (parsed.ec != std::errc{} || parsed.ptr != line.data() + line.size() || count > MaxTrackerDevices)
		return {};
	return count;
}

static void RecordHostLine(HeadsetHostSnapshot& result, std::string line)
{
	if (!line.empty() && line.back() == '\r')
		line.pop_back();
	if (line.empty() || line.starts_with("simxr_host_") || line.starts_with("body_support_"))
		return;
	for (const std::string_view prefix : {"available_extension=", "available_layer="})
		if (line.starts_with(prefix))
		{
			if (result.extensions.size() < 1024)
				result.extensions.push_back(line.substr(prefix.size()) +
											(prefix == "available_layer=" ? " (layer)" : ""));
			return;
		}
	const auto equals = line.find('=');
	if (equals != std::string::npos && equals > 0 &&
		std::ranges::all_of(std::string_view(line).substr(0, equals), [](char character) {
			return std::islower(static_cast<unsigned char>(character)) ||
				   std::isdigit(static_cast<unsigned char>(character)) || character == '_';
		}))
	{
		auto key = line.substr(0, equals), value = line.substr(equals + 1);
		// Per-eye lines read "eye=0 recommended=WxH".
		if (key == "eye")
		{
			const auto space = value.find(' ');
			key += ' ' + value.substr(0, space);
			value = space == std::string::npos ? std::string{} : value.substr(space + 1);
		}
		const auto existing = std::ranges::find(result.details, key, &std::pair<std::string, std::string>::first);
		if (existing != result.details.end())
			existing->second = std::move(value);
		else if (result.details.size() < 256)
			result.details.emplace_back(std::move(key), std::move(value));
		return;
	}
	result.diagnostics += line;
	result.diagnostics += '\n';
	if (result.diagnostics.size() > 65536)
		result.diagnostics.erase(0, result.diagnostics.size() - 65536);
}

static void CheckProcess(bool result)
{
	if (!result)
		throw std::runtime_error(SDL_GetError());
}

HeadsetHost::~HeadsetHost()
{
	Stop();
	if (m_worker.joinable())
		m_worker.join();
}

void HeadsetHost::Start(HeadsetHostOptions options)
{
	if (m_restart)
		throw std::logic_error("Headset restart is already pending");
	const auto current = Snapshot();
	const auto phase = current.phase;
	const bool fromDiscovery = phase == HeadsetHostPhase::Running && current.discovering && !options.discover;
	if (!fromDiscovery && (phase == HeadsetHostPhase::Starting || phase == HeadsetHostPhase::Running ||
						   phase == HeadsetHostPhase::Stopping))
		throw std::logic_error("Disconnect the current headset host before starting another");
	if (options.executable.empty() ||
		(!options.discover && (options.endpoint.empty() || !options.session || !options.generation)))
		throw std::invalid_argument("Headset host requires an executable and explicit simulator target");
	if (options.discover && (!options.endpoint.empty() || options.session || options.generation || options.alignment ||
							 options.localEyeHeight || options.takeOver || options.trackers || options.body))
		throw std::invalid_argument("Headset discovery cannot forward input or claim a simulator target");
	if (options.body && static_cast<std::size_t>(*options.body) >= BodyJointSetNames.size())
		throw std::invalid_argument("Invalid body provider joint set");
	if (options.alignment)
	{
		const auto& value = *options.alignment;
		if (!std::isfinite(value.x) || !std::isfinite(value.z) || !std::isfinite(value.yaw) ||
			!std::isfinite(value.floor))
			throw std::invalid_argument("Headset alignment values must be finite");
	}
	if (options.localEyeHeight &&
		(!options.alignment || !std::isfinite(*options.localEyeHeight) || *options.localEyeHeight <= 0))
		throw std::invalid_argument("LOCAL eye height requires alignment and a positive finite value");
	if (fromDiscovery)
	{
		Stop();
		m_restart = std::move(options);
		return;
	}
	if (m_worker.joinable())
		m_worker.join();
	{
		std::lock_guard lock(m_mutex);
		m_snapshot = {};
		m_snapshot.phase = HeadsetHostPhase::Starting;
		m_snapshot.discovering = options.discover;
	}
	m_target = options;
	try
	{
		m_worker = std::jthread(
			[this, options = std::move(options)](std::stop_token stop) mutable { Run(stop, std::move(options)); });
	}
	catch (...)
	{
		std::lock_guard lock(m_mutex);
		m_snapshot.phase = HeadsetHostPhase::Failed;
		throw;
	}
}

void HeadsetHost::Stop()
{
	m_worker.request_stop();
	m_target.reset();
	m_restart.reset();
}

void HeadsetHost::Realign(HeadsetHostAlignment alignment)
{
	if (!m_target || m_target->discover || m_restart || Snapshot().phase != HeadsetHostPhase::Running)
		throw std::logic_error("Connect the headset before realigning");
	if (!std::isfinite(alignment.x) || !std::isfinite(alignment.z) || !std::isfinite(alignment.yaw) ||
		!std::isfinite(alignment.floor))
		throw std::invalid_argument("Headset alignment values must be finite");
	auto next = *m_target;
	next.alignment = alignment;
	next.takeOver = false;
	Stop();
	m_restart = std::move(next);
}

bool HeadsetHost::Restarting() const
{
	return m_restart.has_value();
}

void HeadsetHost::ObserveTarget(const std::string& endpoint, std::uint64_t session, std::uint64_t generation,
								std::uint64_t connectionEpoch)
{
	const auto matches = [&](const HeadsetHostOptions& target) {
		return target.discover || (target.endpoint == endpoint && target.session == session &&
								   target.generation == generation && target.connectionEpoch == connectionEpoch);
	};
	if ((m_target && !matches(*m_target)) || (m_restart && !matches(*m_restart)))
	{
		Stop();
		return;
	}
	if (m_restart)
	{
		const auto state = Snapshot();
		if (state.phase == HeadsetHostPhase::Failed || state.forcedStop)
			m_restart.reset();
		else if (state.phase == HeadsetHostPhase::Exited)
		{
			auto next = std::move(*m_restart);
			m_restart.reset();
			Start(std::move(next));
		}
	}
}

HeadsetHostSnapshot HeadsetHost::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return m_snapshot;
}

void HeadsetHost::Run(std::stop_token stop, HeadsetHostOptions options)
{
	std::unique_ptr<SDL_Process, decltype(&SDL_DestroyProcess)> process(nullptr, SDL_DestroyProcess);
	HeadsetHostSnapshot result;
	result.discovering = options.discover;
	try
	{
		std::vector<std::string> arguments{PathText(options.executable),
										   "--run",
										   "--parent-control",
										   "--forward-input",
										   options.endpoint,
										   std::to_string(options.session),
										   std::to_string(options.generation),
										   "--game-output"};
		if (options.discover)
			arguments = {PathText(options.executable), "--discover", "--parent-control"};
		if (options.runtimeManifest.empty())
			arguments.emplace_back("--system");
		else
		{
			arguments.emplace_back("--runtime");
			arguments.push_back(PathText(options.runtimeManifest));
		}
		if (options.alignment)
		{
			const auto& value = *options.alignment;
			arguments.emplace_back("--align-player");
			for (const auto number : {value.x, value.z, value.yaw, value.floor})
				arguments.push_back(NumberText(number));
		}
		if (options.localEyeHeight)
		{
			arguments.emplace_back("--local-eye-height");
			arguments.push_back(NumberText(*options.localEyeHeight));
		}
		if (options.trackers)
			arguments.emplace_back("--forward-trackers");
		if (options.body)
		{
			arguments.emplace_back("--forward-body");
			arguments.emplace_back(BodyJointSetNames[static_cast<std::size_t>(*options.body)]);
		}
		if (options.takeOver)
			arguments.emplace_back("--take-over");
		std::vector<const char*> pointers;
		for (const auto& argument : arguments)
		{
			if (argument.find('\0') != std::string::npos)
				throw std::invalid_argument("Headset host argument contains a null byte");
			pointers.push_back(argument.c_str());
		}
		pointers.push_back(nullptr);
		const auto props = SDL_CreateProperties();
		CheckProcess(props != 0);
		try
		{
			CheckProcess(SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, pointers.data()));
			CheckProcess(SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP));
			CheckProcess(SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP));
			CheckProcess(SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true));
			process.reset(SDL_CreateProcessWithProperties(props));
			CheckProcess(process != nullptr);
		}
		catch (...)
		{
			SDL_DestroyProperties(props);
			throw;
		}
		SDL_DestroyProperties(props);
		auto* output = SDL_GetProcessOutput(process.get());
		auto* input = SDL_GetProcessInput(process.get());
		CheckProcess(output && input);
		result.phase = HeadsetHostPhase::Running;
		std::string statusLine, line;
		bool discardLine = false;
		auto statusDeadline = std::chrono::steady_clock::time_point{};
		auto outputDeadline = std::chrono::steady_clock::time_point{};
		auto trackerDeadline = std::chrono::steady_clock::time_point{};
		auto deadline = std::chrono::steady_clock::time_point::max();
		while (true)
		{
			const bool exited = SDL_WaitProcess(process.get(), false, &result.exitCode);
			// Bound each drain as well as retained bytes so a noisy child cannot starve stop.
			for (unsigned chunk = 0; chunk < 16; ++chunk)
			{
				std::array<char, 4096> bytes;
				const auto count = SDL_ReadIO(output, bytes.data(), bytes.size());
				if (!count)
				{
					CheckProcess(SDL_GetIOStatus(output) != SDL_IO_STATUS_ERROR);
					break;
				}
				for (std::size_t index = 0; index < count; ++index)
				{
					const auto byte = bytes[index];
					if (byte == '\n')
					{
						if (!statusLine.empty() && statusLine.back() == '\r')
							statusLine.pop_back();
						std::optional<HeadsetInputState> state;
						std::optional<HeadsetOutputState> display;
						if (!discardLine)
						{
							if (result.phase == HeadsetHostPhase::Running)
								if (const auto sizes = ParseEyeSizes(statusLine))
									result.recommendedEyes = sizes;
							if (result.phase == HeadsetHostPhase::Running)
							{
								for (std::size_t provider = 0; provider < 3; ++provider)
								{
									constexpr std::array names{
										"body_support_fb=", "body_support_meta_full_body=", "body_support_bd="};
									if (statusLine == std::string(names[provider]) + "1")
										result.bodySupport[provider] = true;
									if (statusLine == std::string(names[provider]) + "0")
										result.bodySupport[provider] = false;
								}
								if (statusLine == "simxr_host_tracker_support=1")
									result.trackerSupport = true;
								else if (statusLine == "simxr_host_tracker_support=0")
									result.trackerSupport = false;
								if (const auto count = ParseTrackerCount(statusLine);
									count && options.trackers && result.trackerSupport == true)
								{
									result.connectedTrackers = count;
									trackerDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
								}
							}
							if (statusLine == "simxr_host_input=ready")
								state = HeadsetInputState::Ready;
							else if (statusLine == "simxr_host_input=suspended")
								state = HeadsetInputState::Suspended;
							else if (statusLine == "simxr_host_input=waiting")
								state = HeadsetInputState::Waiting;
							else if (statusLine == "simxr_host_output=live")
								display = HeadsetOutputState::Live;
							else if (statusLine == "simxr_host_output=inspection")
								display = HeadsetOutputState::Inspection;
							else if (statusLine == "simxr_host_output=hidden")
								display = HeadsetOutputState::Hidden;
							else if (statusLine == "simxr_host_output=waiting")
								display = HeadsetOutputState::Waiting;
						}
						if (state && result.phase == HeadsetHostPhase::Running)
						{
							result.input = *state;
							statusDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
						}
						if (display && result.phase == HeadsetHostPhase::Running)
						{
							result.output = *display;
							outputDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
						}
						statusLine.clear();
						discardLine = false;
						RecordHostLine(result, std::exchange(line, {}));
					}
					else
					{
						// An overlong line keeps its end, where a failure usually says what happened.
						if (line.size() == 4096)
							line.erase(0, 2048);
						line.push_back(byte);
						if (discardLine)
							continue;
						if (statusLine.size() == 128)
							discardLine = true;
						else
							statusLine.push_back(byte);
					}
				}
			}
			if (std::chrono::steady_clock::now() >= statusDeadline)
				result.input = HeadsetInputState::Waiting;
			if (std::chrono::steady_clock::now() >= outputDeadline)
				result.output = HeadsetOutputState::Waiting;
			if (std::chrono::steady_clock::now() >= trackerDeadline)
				result.connectedTrackers.reset();
			if (exited)
			{
				// Keep a last line the host ended without a newline, such as a failure message.
				RecordHostLine(result, std::exchange(line, {}));
				result.phase =
					result.exitCode == 0 && !result.forcedStop ? HeadsetHostPhase::Exited : HeadsetHostPhase::Failed;
				break;
			}
			if (stop.stop_requested() && input)
			{
				CheckProcess(SDL_CloseIO(input));
				input = nullptr;
				result.phase = HeadsetHostPhase::Stopping;
				result.recommendedEyes.reset();
				result.trackerSupport.reset();
				result.bodySupport = {};
				result.connectedTrackers.reset();
				result.input = HeadsetInputState::Waiting;
				result.output = HeadsetOutputState::Waiting;
				deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
			}
			if (!result.forcedStop && std::chrono::steady_clock::now() >= deadline)
			{
				CheckProcess(SDL_KillProcess(process.get(), true));
				result.forcedStop = true;
				result.error =
					"Headset host did not stop within six seconds; forced termination does not prove GPU completion";
			}
			{
				std::lock_guard lock(m_mutex);
				m_snapshot = result;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}
	catch (const std::exception& error)
	{
		result.phase = HeadsetHostPhase::Failed;
		result.error = error.what();
		if (process && !SDL_WaitProcess(process.get(), false, nullptr))
		{
			result.forcedStop = true;
			if (SDL_KillProcess(process.get(), true))
				SDL_WaitProcess(process.get(), true, nullptr);
			else
				result.error += std::string("; cannot terminate headset host: ") + SDL_GetError();
		}
	}
	process.reset();
	result.recommendedEyes.reset();
	result.trackerSupport.reset();
	result.bodySupport = {};
	result.connectedTrackers.reset();
	result.input = HeadsetInputState::Waiting;
	result.output = HeadsetOutputState::Waiting;
	std::lock_guard lock(m_mutex);
	m_snapshot = std::move(result);
}
} // namespace simxr::app
