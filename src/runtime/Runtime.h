#pragma once

#include "platform/Clock.h"
#include "runtime/ApiVersion.h"
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
	XrVersion apiVersion = SupportedApiVersion;
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
	GameBridge& Game();

	std::shared_ptr<DebugController> Debugger(XrSession session, const DebugControllerConfig& configuration = {});
	void CloseDebugger(XrSession session);

	void Close();

private:
	struct DebugService;
	using DebugServices = std::unordered_map<XrSession, std::shared_ptr<DebugService>>;

	const ExtensionRegistry m_extensions;
	const RuntimeConfiguration m_configuration;
	mutable std::mutex m_mutex;
	HandleRegistry<XrInstance, Instance, std::uint32_t> m_instances;
	HandleRegistry<XrSession, Session, XrInstance> m_sessions;
	std::unique_ptr<DebugServices> m_debuggers = std::make_unique<DebugServices>();
	GameBridge m_game;
	bool m_closed = false;
};
} // namespace simxr
