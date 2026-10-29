#pragma once

#include "control/legacy/LegacyInput.h"
#include "graphics/backends/vulkan/VulkanInstanceGraphics.h"
#include "graphics/backends/vulkan/VulkanSessionGraphics.h"
#include "platform/linux/RuntimeFiles.h"
#include "runtime/NativeBindings.h"
#include "runtime/Runtime.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace simxr::control
{
class ControlService;
class LocalControlServer;
} // namespace simxr::control
namespace simxr
{

class LinuxInstance final
{
  public:
	LinuxInstance(Runtime::InstanceRef instance, const std::filesystem::path& directory, ExecutionMode executionMode);
	~LinuxInstance();
	LinuxInstance(const LinuxInstance&) = delete;
	LinuxInstance& operator=(const LinuxInstance&) = delete;
	const LegacyInput& Input() const noexcept;
	void Log(const std::string& message);
	void Close();

	const Runtime::InstanceRef core;
	const ExecutionMode mode;
	VulkanInstanceGraphics graphics;

  private:
	friend class LinuxRuntime;
	LegacyInput m_input;
	const std::shared_ptr<const PoseSequence> m_startupSequence;
	RuntimeLog m_log;
};

// Owns native bindings and transport; the portable Runtime owns XR identities.
// Instance state uses Mutex. Session graphics have a separate native gate; pacing
// and GPU drain run outside both. Never acquire a graphics gate while holding Mutex.
class LinuxRuntime final
{
  public:
	using InstanceRef = HandleRef<XrInstance, LinuxInstance>;
	struct SessionBinding
	{
		SessionBinding(InstanceRef instance, Runtime::SessionRef session,
					   std::unique_ptr<VulkanSessionGraphics> native);
		const InstanceRef owner;
		const Runtime::SessionRef core;
		std::mutex graphicsMutex;
		const std::unique_ptr<VulkanSessionGraphics> graphics;
	};
	using SessionRef = HandleRef<XrSession, SessionBinding>;

	LinuxRuntime();
	~LinuxRuntime();
	LinuxRuntime(const LinuxRuntime&) = delete;
	LinuxRuntime& operator=(const LinuxRuntime&) = delete;
	static const ExtensionRegistry& Extensions();
	Runtime& Core() noexcept;
	std::recursive_mutex& Mutex() noexcept;
	InstanceRef FindInstance(XrInstance handle) const;
	SessionRef FindSession(XrSession handle) const;
	InstanceRef CurrentInstance() const;
	SessionRef CurrentSession() const;
	XrResult CreateInstance(const XrInstanceCreateInfo* info, XrInstance* output);
	XrResult DestroyInstance(XrInstance handle);
	XrResult CreateSession(XrInstance owner, const XrSessionCreateInfo* info, XrSession* output);
	XrResult DestroySession(XrSession handle);
	XrResult BeginSession(XrSession handle, const XrSessionBeginInfo* info);
	XrResult EndSession(XrSession handle);
	void StartControlEndpoint();
	bool Shutdown();
	static RuntimeTime SessionTime(const SessionBinding& session);

  private:
	void CloseControlEndpoint() noexcept;
	const std::filesystem::path& DataDirectory();

	Runtime m_core;
	std::once_flag m_dataDirectoryOnce;
	std::filesystem::path m_dataDirectory;
	mutable std::recursive_mutex m_mutex;
	std::mutex m_lifecycleMutex;
	NativeBindings<XrInstance, Instance, LinuxInstance> m_instances;
	NativeBindings<XrSession, Session, SessionBinding> m_sessions;
	XrInstance m_currentInstance = XR_NULL_HANDLE;
	XrSession m_currentSession = XR_NULL_HANDLE;
	std::mutex m_endpointMutex;
	std::unique_ptr<control::ControlService> m_control;
	std::unique_ptr<control::LocalControlServer> m_endpoint;
	bool m_endpointStarted = false;
	bool m_closed = false;
};
} // namespace simxr
