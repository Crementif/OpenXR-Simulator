#define XR_USE_TIMESPEC
#define XR_USE_GRAPHICS_API_VULKAN
#include "platform/linux/LinuxRuntime.h"
#include "Pch.h"

#include "control/ControlService.h"
#include "control/LocalControlServer.h"
#include "control/backends/vulkan/VulkanImageInspection.h"
#include "runtime/SimulationExtensions.h"
#include "simulation/HeadsetProfiles.h"

#include <openxr/openxr_platform.h>
#include <ctime>
#include <unistd.h>

namespace simxr
{
static XrTime MonotonicTime()
{
	timespec time{};
	if (clock_gettime(CLOCK_MONOTONIC, &time) != 0)
		throw std::system_error(errno, std::generic_category(), "Read monotonic runtime clock");
	if (time.tv_sec < 0 || time.tv_nsec < 0 || time.tv_nsec >= 1000000000 ||
		time.tv_sec > (std::numeric_limits<XrTime>::max() - time.tv_nsec) / 1000000000LL)
		throw std::overflow_error("Monotonic runtime clock exhausted");
	return time.tv_sec * 1000000000LL + time.tv_nsec;
}

LinuxInstance::LinuxInstance(Runtime::InstanceRef instance, const std::filesystem::path& directory,
							 ExecutionMode executionMode)
	: core(std::move(instance)), mode(executionMode),
	  m_startupSequence(LoadStartupSequence(std::getenv("SIMXR_SEQUENCE") ? std::getenv("SIMXR_SEQUENCE") : "")),
	  m_log(directory)
{
	auto views = m_input.Views();
	if (const auto* name = std::getenv("SIMXR_HEADSET_PROFILE"))
	{
		const auto* profile = FindHeadsetProfile(name);
		if (!profile)
			throw std::invalid_argument("Unknown SIMXR_HEADSET_PROFILE");
		views = ProfileViews(*profile);
	}
	if (const auto* value = std::getenv("SIMXR_WIDTH"))
		views.outerWidth = ParseLaunchRenderExtent(value);
	if (const auto* value = std::getenv("SIMXR_HEIGHT"))
		views.outerHeight = ParseLaunchRenderExtent(value);
	m_input.SetViews(views);
}

LinuxInstance::~LinuxInstance()
{
	Close();
}

const LegacyInput& LinuxInstance::Input() const noexcept
{
	return m_input;
}

void LinuxInstance::Log(const std::string& message)
{
	m_log.Log(message);
}

void LinuxInstance::Close()
{
	m_log.Close();
}

LinuxRuntime::SessionBinding::SessionBinding(InstanceRef instance, Runtime::SessionRef session,
											 std::unique_ptr<VulkanSessionGraphics> native)
	: owner(std::move(instance)), core(std::move(session)), graphics(std::move(native))
{}

LinuxRuntime::LinuxRuntime() : m_core(Extensions()) {}

LinuxRuntime::~LinuxRuntime()
{
	CloseControlEndpoint();
	m_core.Close();
}

const ExtensionRegistry& LinuxRuntime::Extensions()
{
	static constexpr std::array<std::string_view, 4> s_vulkanCommands{
		"xrGetVulkanInstanceExtensionsKHR", "xrGetVulkanDeviceExtensionsKHR", "xrGetVulkanGraphicsDeviceKHR",
		"xrGetVulkanGraphicsRequirementsKHR"};
	static constexpr std::array<std::string_view, 4> s_vulkan2Commands{
		"xrCreateVulkanInstanceKHR", "xrCreateVulkanDeviceKHR", "xrGetVulkanGraphicsDevice2KHR",
		"xrGetVulkanGraphicsRequirements2KHR"};
	static constexpr std::array<std::string_view, 2> s_timeCommands{"xrConvertTimespecTimeToTimeKHR",
																	"xrConvertTimeToTimespecTimeKHR"};
	static const std::vector<ExtensionDefinition> s_definitions = [] {
		std::vector<ExtensionDefinition> result{
			{XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_vulkan_enable_SPEC_VERSION, s_vulkanCommands},
			{XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, XR_KHR_composition_layer_depth_SPEC_VERSION, {}},
			{XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME, XR_KHR_convert_timespec_time_SPEC_VERSION, s_timeCommands}};
		const auto simulation = SimulationExtensions({true, true});
		result.insert(result.end(), simulation.begin(), simulation.end());
		if (!std::getenv("SIMXR_VULKAN_V1_ONLY"))
			result.push_back(
				{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_vulkan_enable2_SPEC_VERSION, s_vulkan2Commands});
		return result;
	}();
	static const ExtensionRegistry s_registry(s_definitions);
	return s_registry;
}

RuntimeTime LinuxRuntime::SessionTime(const SessionBinding& session)
{
	if (session.owner->mode == ExecutionMode::Deterministic)
		return session.core->Snapshot().time;
	return {Duration{MonotonicTime()}};
}

Runtime& LinuxRuntime::Core() noexcept
{
	return m_core;
}

std::recursive_mutex& LinuxRuntime::Mutex() noexcept
{
	return m_mutex;
}

LinuxRuntime::InstanceRef LinuxRuntime::FindInstance(XrInstance handle) const
{
	return m_instances.Find(m_core.FindInstance(handle));
}

LinuxRuntime::SessionRef LinuxRuntime::FindSession(XrSession handle) const
{
	return m_sessions.Find(m_core.FindSession(handle));
}

LinuxRuntime::InstanceRef LinuxRuntime::CurrentInstance() const
{
	std::lock_guard lock(m_mutex);
	return FindInstance(m_currentInstance);
}

LinuxRuntime::SessionRef LinuxRuntime::CurrentSession() const
{
	std::lock_guard lock(m_mutex);
	return FindSession(m_currentSession);
}

XrResult LinuxRuntime::CreateInstance(const XrInstanceCreateInfo* info, XrInstance* output)
{
	std::lock_guard lifecycle(m_lifecycleMutex);
	if (!output)
		return XR_ERROR_VALIDATION_FAILURE;
	Runtime::InstanceRef core;
	const auto result = m_core.CreateInstance(info, core);
	if (XR_FAILED(result))
		return result;

	struct Creation
	{
		Runtime& runtime;
		Runtime::InstanceRef core;
		bool committed = false;

		~Creation()
		{
			if (!committed)
				runtime.DestroyInstance(core);
		}
	} creation{m_core, core};

	const auto* deterministic = std::getenv("SIMXR_DETERMINISTIC");
	const auto mode =
		deterministic && std::strcmp(deterministic, "0") != 0 ? ExecutionMode::Deterministic : ExecutionMode::RealTime;
	auto native = std::make_unique<LinuxInstance>(core, DataDirectory(), mode);
	native->Log(std::string("create instance app=") + info->applicationInfo.applicationName);
	StartControlEndpoint();
	const auto attached = m_instances.Attach(core, std::move(native));
	std::lock_guard lock(m_mutex);
	m_currentInstance = attached;
	*output = attached;
	creation.committed = true;
	return XR_SUCCESS;
}

XrResult LinuxRuntime::DestroyInstance(XrInstance handle)
{
	std::lock_guard lifecycle(m_lifecycleMutex);
	const auto instance = FindInstance(handle);
	std::unique_lock lock(m_mutex);
	if (!FindInstance(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (const auto session = CurrentSession())
	{
		lock.unlock();
		const auto result = DestroySession(session);
		if (XR_FAILED(result))
			return result;
		lock.lock();
	}
	instance->Log("destroy instance");
	m_core.DestroyInstance(instance);
	m_instances.Remove(instance);
	m_currentInstance = XR_NULL_HANDLE;
	lock.unlock();
	instance->Close();
	return XR_SUCCESS;
}

XrResult LinuxRuntime::CreateSession(XrInstance handle, const XrSessionCreateInfo* info, XrSession* output)
{
	std::lock_guard lifecycle(m_lifecycleMutex);
	const auto instance = FindInstance(handle);
	std::unique_lock lock(m_mutex);
	if (!FindInstance(instance))
		return XR_ERROR_HANDLE_INVALID;
	if (CurrentSession())
		return XR_ERROR_LIMIT_REACHED;
	if (!info || !output || info->type != XR_TYPE_SESSION_CREATE_INFO)
		return XR_ERROR_VALIDATION_FAILURE;
	if (info->systemId != 1)
		return XR_ERROR_SYSTEM_INVALID;
	const auto& extensions = instance->core->Extensions();
	if (!extensions.Contains(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) &&
		!extensions.Contains(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	VulkanBinding binding{};
	const auto validated = instance->graphics.SessionBinding(*info, binding);
	if (XR_FAILED(validated))
		return validated;
	Runtime::SessionRef core;
	auto result = m_core.CreateSession(instance->core, core);
	if (XR_FAILED(result))
		return result;
	lock.unlock();

	struct Creation
	{
		Runtime& runtime;
		Runtime::SessionRef core;
		bool committed = false;

		~Creation()
		{
			if (!committed)
				runtime.DestroySession(core);
		}
	} creation{m_core, core};

	std::unique_ptr<VulkanSessionGraphics> graphics;
	const auto* requestedValidation = std::getenv("SIMXR_VULKAN_VALIDATION");
	const bool validation = requestedValidation && std::strcmp(requestedValidation, "1") == 0;
	result = VulkanSessionGraphics::Create(binding, core.Shared(), validation, graphics);
	if (XR_FAILED(result))
		return result;

	auto native = std::make_unique<SessionBinding>(instance, core, std::move(graphics));
	result = core->Ready({Duration{instance->mode == ExecutionMode::Deterministic ? 1000000000LL : MonotonicTime()}});
	if (XR_FAILED(result))
		return result;
	instance->Log("create Vulkan session queueFamily=" + std::to_string(binding.queueFamily) +
				  " queueIndex=" + std::to_string(binding.queueIndex));
	const auto attached = m_sessions.Attach(core, std::move(native));
	lock.lock();
	m_currentSession = attached;
	*output = attached;
	creation.committed = true;
	return XR_SUCCESS;
}

XrResult LinuxRuntime::BeginSession(XrSession handle, const XrSessionBeginInfo* info)
{
	auto session = FindSession(handle);
	std::unique_lock lock(m_mutex);
	if (!FindSession(session))
		return XR_ERROR_HANDLE_INVALID;
	if (!info)
		return XR_ERROR_VALIDATION_FAILURE;
	FrameSchedulerConfig config;
	config.epoch = {Duration{session->owner->mode == ExecutionMode::Deterministic ? 1000000000LL : MonotonicTime()}};
	config.mode = session->owner->mode;
	if (const auto* text = std::getenv("SIMXR_APPLICATION_CADENCE"))
	{
		const std::string_view value{text};
		if (value == "auto")
			config.automaticCadence = true;
		else if (value == "1" || value == "2")
			config.cadence = value == "2" ? 2 : 1;
		else
			return XR_ERROR_VALIDATION_FAILURE;
	}
	std::uint64_t seed = 0;
	if (const auto* text = std::getenv("SIMXR_SEED"))
	{
		const std::string_view value{text};
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seed);
		if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
			return XR_ERROR_VALIDATION_FAILURE;
	}
	PresentationPolicy policy = PresentationPolicy::Repeat;
	if (const auto* text = std::getenv("SIMXR_PRESENTATION_POLICY"))
	{
		const std::string_view value{text};
		if (value == "rotational")
			policy = PresentationPolicy::Rotational;
		else if (value != "repeat")
			return XR_ERROR_VALIDATION_FAILURE;
	}
	if (policy == PresentationPolicy::Rotational && !session->graphics->SupportsReprojection())
		return XR_ERROR_FEATURE_UNSUPPORTED;
	CommandConfiguration baseline;
	baseline.tracking = session->owner->Input().Tracking();
	baseline.input = session->core->Input().Snapshot();
	baseline.input.controllers = session->owner->Input().Controls();
	baseline.views = session->owner->Input().Views();
	auto configured = session->core->ConfigureCommands(seed, baseline, false);
	if (XR_FAILED(configured))
		return configured;
	lock.unlock();
	return session->core->Begin(*info, config, policy, session->owner->m_startupSequence);
}

XrResult LinuxRuntime::EndSession(XrSession handle)
{
	const auto session = FindSession(handle);
	std::unique_lock lock(m_mutex);
	if (!FindSession(session))
		return XR_ERROR_HANDLE_INVALID;
	const auto time = SessionTime(*session.Shared());
	lock.unlock();
	const auto result = session->core->End(time);
	return result;
}

XrResult LinuxRuntime::DestroySession(XrSession handle)
{
	const auto session = FindSession(handle);
	if (!session)
		return XR_ERROR_HANDLE_INVALID;
	// Cancel CPU admission before waiting for an in-flight graphics call.
	session->core->Close();
	m_core.CloseDebugger(session);
	{
		std::lock_guard nativeLock(session->graphicsMutex);
		if (!FindSession(session))
			return XR_ERROR_HANDLE_INVALID;
		session->graphics->BeginClose();
	}
	const auto drained = session->graphics->Drain();
	if (XR_FAILED(drained))
		return drained;
	std::lock_guard nativeLock(session->graphicsMutex);
	if (!FindSession(session))
		return XR_ERROR_HANDLE_INVALID;
	session->core->Display().PollRetired();
	session->core->Captures().Poll();
	const auto closed = session->graphics->FinishClose();
	if (XR_FAILED(closed))
		return closed;
	std::lock_guard lock(m_mutex);
	m_core.DestroySession(session);
	m_sessions.Remove(session);
	m_currentSession = XR_NULL_HANDLE;
	return XR_SUCCESS;
}

void LinuxRuntime::StartControlEndpoint()
{
	std::lock_guard lock(m_endpointMutex);
	if (m_endpointStarted || m_closed)
		return;
	m_control = std::make_unique<control::ControlService>(
		m_core, 16, control::CreateVulkanImageInspectionProvider([this](XrSession handle) {
			const auto native = FindSession(handle);
			return native ? native->graphics->Capabilities() : control::ImageCapabilities{};
		}));
	m_endpointStarted = true;
	try
	{
		const auto* selected = std::getenv("SIMXR_CONTROL_SOCKET");
		const auto path = selected && *selected
							  ? std::string(selected)
							  : (DataDirectory() / ("control." + std::to_string(getpid()) + ".sock")).string();
		m_endpoint = std::make_unique<control::LocalControlServer>(*m_control, path);
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "SimXR: local control endpoint unavailable: %s\n", error.what());
	}
}

bool LinuxRuntime::Shutdown()
{
	std::lock_guard lifecycle(m_lifecycleMutex);
	if (!m_core.Instances().empty())
		return false;
	CloseControlEndpoint();
	m_core.Close();
	return true;
}

void LinuxRuntime::CloseControlEndpoint() noexcept
{
	std::lock_guard lock(m_endpointMutex);
	m_closed = true;
	if (m_endpoint)
		m_endpoint->Close();
	if (m_control)
		m_control->Close();
}

static std::filesystem::path ReadDataDirectory()
{
	const char* explicitPath = std::getenv("SIMXR_DATA_DIR");
	if (explicitPath && *explicitPath)
		return explicitPath;
	const char* base = std::getenv("XDG_DATA_HOME");
	if (base && *base)
		return std::filesystem::path(base) / "OpenXR-Simulator";
	const char* home = std::getenv("HOME");
	return std::filesystem::path(home ? home : ".") / ".local/share/OpenXR-Simulator";
}

const std::filesystem::path& LinuxRuntime::DataDirectory()
{
	std::call_once(m_dataDirectoryOnce, [this] { m_dataDirectory = ReadDataDirectory(); });
	return m_dataDirectory;
}

} // namespace simxr
