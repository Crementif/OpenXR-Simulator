#pragma once

#include "platform/Clock.h"
#include "presentation/DebugController.h"
#include "runtime/ExtensionRegistry.h"
#include "runtime/HandleRegistry.h"
#include "runtime/Instance.h"
#include "runtime/Session.h"
#include "simulation/GameBridge.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace simxr
{
struct RuntimeConfiguration
{
	XrVersion apiVersion = XR_MAKE_VERSION(1, 0, 27);
	std::size_t maxInstances = 1;
	std::size_t maxSessionsPerInstance = 1;
};

class Runtime
{
  public:
	using InstanceRef = HandleRef<XrInstance, Instance>;
	using SessionRef = HandleRef<XrSession, Session>;

	Runtime(const ExtensionRegistry& extensions, const RuntimeConfiguration& configuration = {});
	~Runtime();
	XrResult CreateInstance(const XrInstanceCreateInfo* info, InstanceRef& output);
	XrResult DestroyInstance(XrInstance instance);
	XrResult CreateSession(XrInstance owner, SessionRef& output,
						   std::shared_ptr<Clock> clock = std::make_shared<SteadyClock>());
	XrResult DestroySession(XrSession session);
	InstanceRef FindInstance(XrInstance instance) const;
	SessionRef FindSession(XrSession session) const;
	SessionRef FindSession(XrSession session, XrInstance owner) const;
	std::vector<XrInstance> Instances() const;
	std::vector<XrSession> Sessions(XrInstance owner) const;
	RuntimeConfiguration Configuration() const;
	std::shared_ptr<DebugController> Debugger(XrSession session, const DebugControllerConfig& configuration = {});
	GameBridge& Game();
	void CloseDebugger(XrSession session);
	void Close();

  private:
	const ExtensionRegistry m_extensions;
	const RuntimeConfiguration m_configuration;
	mutable std::mutex m_mutex;
	HandleRegistry<XrInstance, Instance, std::uint32_t> m_instances;
	HandleRegistry<XrSession, Session, XrInstance> m_sessions;
	struct DebugService;
	std::unordered_map<XrSession, std::shared_ptr<DebugService>> m_debuggers;
	GameBridge m_game;
	bool m_closed = false;
};
} // namespace simxr
