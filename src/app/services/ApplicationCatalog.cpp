#include "Pch.h"
#include "app/services/ApplicationCatalog.h"
#include "app/services/MonitorController.h"

namespace simxr::app
{
ApplicationCatalog::ApplicationCatalog()
{
	if (const auto* target = std::getenv("SIMXR_MONITOR"))
		m_monitorTarget = target;
	if (!m_monitorTarget.empty() && m_monitorTarget != "composed" && m_monitorTarget != "ui")
		throw std::invalid_argument("SIMXR_MONITOR must be composed or ui");
#ifndef SIMXR_COMPANION_AUTOMATION
	if (!m_monitorTarget.empty())
		throw std::runtime_error("Configured monitoring requires Luau automation support");
#endif
}
ApplicationCatalog::~ApplicationCatalog() = default;
void ApplicationCatalog::StartMonitor(std::uint64_t id, MonitorOptions options)
{
#ifdef SIMXR_COMPANION_AUTOMATION
	const auto applications = Applications();
	const auto application = std::ranges::find(applications, id, &ApplicationSnapshot::id);
	if (application == applications.end())
		throw std::runtime_error("Application is no longer available");
	auto found = m_monitors.find(id);
	const auto active = std::ranges::count_if(m_monitors, [](const auto& item) { return item.second->Active(); });
	if (active >= 4 && (found == m_monitors.end() || !found->second->Active()))
		throw std::runtime_error("Automatic monitoring capacity reached (four applications)");
	if (found == m_monitors.end())
		found = m_monitors.emplace(id, std::make_unique<MonitorController>()).first;
	found->second->Start(*application, std::move(options));
#else
	throw std::runtime_error("Automatic monitoring requires Luau automation support");
#endif
}
MonitorStatus ApplicationCatalog::ReadMonitor(std::uint64_t id) const
{
#ifdef SIMXR_COMPANION_AUTOMATION
	if (const auto found = m_monitors.find(id); found != m_monitors.end())
		return found->second->Status();
#endif
	return {};
}
void ApplicationCatalog::StopMonitor(std::uint64_t id, const std::string& monitor)
{
#ifdef SIMXR_COMPANION_AUTOMATION
	if (const auto found = m_monitors.find(id); found != m_monitors.end() && found->second->Status().options.id == monitor)
	{
		found->second->Stop();
		return;
	}
#endif
	throw std::invalid_argument("Unknown monitor_id for this application");
}
void ApplicationCatalog::PollMonitors()
{
#ifdef SIMXR_COMPANION_AUTOMATION
	const auto applications = Applications();
	for (auto item = m_monitors.begin(); item != m_monitors.end();)
	{
		const auto application = std::ranges::find(applications, item->first, &ApplicationSnapshot::id);
		if (application == applications.end())
		{
			item = m_monitors.erase(item);
			continue;
		}
		if (!item->second->Matches(*application) || !item->second->Active())
			item->second->Stop();
		++item;
	}
	if (m_monitorTarget.empty())
		return;
	for (const auto& application : applications)
	{
		if (!application.running || !application.connected)
			continue;
		const auto found = m_monitors.find(application.id);
		// A stopped or failed monitor stays terminal for this session. A new session starts fresh.
		if (found != m_monitors.end() && found->second->Matches(application))
			continue;
		if (std::ranges::count_if(m_monitors, [](const auto& item) { return item.second->Active(); }) >= 4)
			continue;
		StartMonitor(application.id, {"configured-" + std::to_string(application.runtimeProcess) + "-" +
			std::to_string(application.runtimeEpoch) + "-" + std::to_string(application.session) + "-" +
			std::to_string(application.generation), m_monitorTarget});
	}
#endif
}
}
