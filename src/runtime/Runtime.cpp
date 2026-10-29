#include "runtime/Runtime.h"
#include "Pch.h"

#include "runtime/validation/StructureChain.h"

namespace simxr
{
struct Runtime::DebugService
{
	std::mutex m_mutex;
	std::shared_ptr<DebugController> m_controller;
	bool m_closed = false;

	std::shared_ptr<DebugController> Get(const SessionRef& session, const DebugControllerConfig& configuration)
	{
		std::lock_guard lock(m_mutex);
		if (m_closed || session->Snapshot().closed)
			return {};
		if (!m_controller)
			m_controller = std::make_shared<DebugController>(session.Shared(), session->ClockSource(), configuration);
		return m_controller;
	}

	void Close()
	{
		std::shared_ptr<DebugController> retired;
		{
			std::lock_guard lock(m_mutex);
			m_closed = true;
			retired = std::move(m_controller);
		}
		if (retired)
			retired->Close(false);
	}
};

Runtime::Runtime(const ExtensionRegistry& extensions, const RuntimeConfiguration& configuration)
	: m_extensions(extensions), m_configuration(configuration)
{
	if (!configuration.maxInstances || !configuration.maxSessionsPerInstance || !configuration.apiVersion)
		throw std::invalid_argument("Invalid runtime configuration");
}

GameBridge& Runtime::Game()
{
	return m_game;
}

Runtime::~Runtime()
{
	Close();
}

XrResult Runtime::CreateInstance(const XrInstanceCreateInfo* info, InstanceRef& output)
{
	if (!info || info->type != XR_TYPE_INSTANCE_CREATE_INFO || info->createFlags ||
		(info->enabledExtensionCount && !info->enabledExtensionNames) ||
		(info->enabledApiLayerCount && !info->enabledApiLayerNames))
		return XR_ERROR_VALIDATION_FAILURE;
	auto result = StructureChain(info->next).Validate({});
	if (XR_FAILED(result))
		return result;
	if (!info->applicationInfo.applicationName[0])
		return XR_ERROR_NAME_INVALID;
	if (std::find(std::begin(info->applicationInfo.applicationName), std::end(info->applicationInfo.applicationName),
				  '\0') == std::end(info->applicationInfo.applicationName) ||
		std::find(std::begin(info->applicationInfo.engineName), std::end(info->applicationInfo.engineName), '\0') ==
			std::end(info->applicationInfo.engineName))
		return XR_ERROR_VALIDATION_FAILURE;
	const auto version = info->applicationInfo.apiVersion;
	if (XR_VERSION_MAJOR(version) != XR_VERSION_MAJOR(m_configuration.apiVersion) ||
		XR_VERSION_MINOR(version) > XR_VERSION_MINOR(m_configuration.apiVersion))
		return XR_ERROR_API_VERSION_UNSUPPORTED;
	const std::span<const char* const> requested(info->enabledExtensionNames, info->enabledExtensionCount);
	for (auto name : requested)
	{
		if (!name)
			return XR_ERROR_VALIDATION_FAILURE;
		std::size_t length = 0;
		while (length < XR_MAX_EXTENSION_NAME_SIZE && name[length])
			++length;
		if (length == XR_MAX_EXTENSION_NAME_SIZE)
			return XR_ERROR_VALIDATION_FAILURE;
	}
	result = m_extensions.Validate(requested, version);
	if (XR_FAILED(result))
		return result;
	InstanceRef created;
	{
		std::lock_guard lock(m_mutex);
		if (m_closed)
			return XR_ERROR_RUNTIME_FAILURE;
		if (m_instances.Children(0).size() >= m_configuration.maxInstances)
			return XR_ERROR_LIMIT_REACHED;
		created = m_instances.Create(
			0, [&](XrInstance handle) { return std::make_unique<Instance>(handle, requested, version); });
	}
	output = std::move(created);
	return XR_SUCCESS;
}

XrResult Runtime::CreateSession(XrInstance owner, SessionRef& output, std::shared_ptr<Clock> clock)
{
	if (!clock)
		return XR_ERROR_VALIDATION_FAILURE;
	SessionRef created;
	{
		std::lock_guard lock(m_mutex);
		auto instance = m_instances.Find(owner);
		if (!instance)
			return XR_ERROR_HANDLE_INVALID;
		if (m_sessions.Children(owner).size() >= m_configuration.maxSessionsPerInstance)
			return XR_ERROR_LIMIT_REACHED;
		created = m_sessions.Create(owner, [&](XrSession handle) {
			return instance->CreateSession(handle, instance.Shared(), std::move(clock));
		});
	}
	output = std::move(created);
	return XR_SUCCESS;
}

XrResult Runtime::DestroySession(XrSession handle)
{
	SessionRef retired;
	std::shared_ptr<DebugService> debugger;
	{
		std::lock_guard lock(m_mutex);
		retired = m_sessions.Find(handle);
		if (!retired)
			return XR_ERROR_HANDLE_INVALID;
		m_sessions.Erase(handle);
		if (const auto entry = m_debuggers->find(handle); entry != m_debuggers->end())
		{
			debugger = std::move(entry->second);
			m_debuggers->erase(entry);
		}
	}
	retired->Close();
	if (debugger)
		debugger->Close();
	return XR_SUCCESS;
}

XrResult Runtime::DestroyInstance(XrInstance handle)
{
	InstanceRef retired;
	std::vector<SessionRef> sessions;
	std::vector<std::shared_ptr<DebugService>> debuggers;
	{
		std::lock_guard lock(m_mutex);
		retired = m_instances.Find(handle);
		if (!retired)
			return XR_ERROR_HANDLE_INVALID;
		const auto children = m_sessions.Children(handle);
		sessions.reserve(children.size());
		debuggers.reserve(children.size());
		for (auto child : children)
			sessions.push_back(m_sessions.Find(child));
		for (auto child : children)
		{
			m_sessions.Erase(child);
			if (const auto entry = m_debuggers->find(child); entry != m_debuggers->end())
			{
				debuggers.push_back(std::move(entry->second));
				m_debuggers->erase(entry);
			}
		}
		m_instances.Erase(handle);
	}
	for (auto& session : sessions)
		session->Close();
	for (auto& debugger : debuggers)
		debugger->Close();
	return XR_SUCCESS;
}

Runtime::InstanceRef Runtime::FindInstance(XrInstance handle) const
{
	std::lock_guard lock(m_mutex);
	return m_instances.Find(handle);
}

Runtime::SessionRef Runtime::FindSession(XrSession handle) const
{
	std::lock_guard lock(m_mutex);
	return m_sessions.Find(handle);
}

Runtime::SessionRef Runtime::FindSession(XrSession handle, XrInstance owner) const
{
	std::lock_guard lock(m_mutex);
	return m_sessions.Find(handle, owner);
}

std::vector<XrInstance> Runtime::Instances() const
{
	std::lock_guard lock(m_mutex);
	return m_instances.Children(0);
}

std::vector<XrSession> Runtime::Sessions(XrInstance owner) const
{
	std::lock_guard lock(m_mutex);
	return m_sessions.Children(owner);
}

RuntimeConfiguration Runtime::Configuration() const
{
	return m_configuration;
}

std::shared_ptr<DebugController> Runtime::Debugger(XrSession handle, const DebugControllerConfig& configuration)
{
	SessionRef session;
	std::shared_ptr<DebugService> debugger;
	{
		std::lock_guard lock(m_mutex);
		session = m_sessions.Find(handle);
		if (!session || m_closed)
			return {};
		auto entry = m_debuggers->find(handle);
		if (entry == m_debuggers->end())
			entry = m_debuggers->emplace(handle, std::make_shared<DebugService>()).first;
		debugger = entry->second;
	}
	return debugger->Get(session, configuration);
}

void Runtime::CloseDebugger(XrSession handle)
{
	std::shared_ptr<DebugService> debugger;
	{
		std::lock_guard lock(m_mutex);
		if (!m_debuggers)
			return;
		if (const auto entry = m_debuggers->find(handle); entry != m_debuggers->end())
		{
			debugger = std::move(entry->second);
			m_debuggers->erase(entry);
		}
	}
	if (debugger)
		debugger->Close();
}

void Runtime::Close()
{
	decltype(m_instances)::Retired instances;
	decltype(m_sessions)::Retired sessions;
	std::unique_ptr<DebugServices> debuggers;
	{
		std::lock_guard lock(m_mutex);
		if (m_closed)
			return;
		instances = m_instances.RetireAll();
		sessions = m_sessions.RetireAll();
		debuggers = std::move(m_debuggers);
		m_closed = true;
	}
	m_game.Close();
	sessions.ForEach([](const SessionRef& session) { session->Close(); });
	for (auto& [handle, debugger] : *debuggers)
		debugger->Close();
}
} // namespace simxr
