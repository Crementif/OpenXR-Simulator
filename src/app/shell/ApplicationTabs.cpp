#include "app/shell/ApplicationTabs.h"
#include "app/UiPch.h"
#include "app/RuntimePanels.h"
#include "app/WorkspacePanels.h"
#include "app/platform/Window.h"
#include "app/render/UiRenderer.h"
#include "app/services/ProcessSupervisor.h"
#include "app/widgets/Icons.h"
#include "app/widgets/Theme.h"
#ifdef SIMXR_COMPANION_AUTOMATION
#include "app/services/LuauAutomation.h"
#endif

namespace simxr::app
{
struct ApplicationTabs::Tab
{
	std::uint64_t id;
	WorkspacePanels workspace;
	RuntimePanels runtime;
	std::uint64_t process = 0;
	bool closing = false, retiring = false;
	std::chrono::steady_clock::time_point retirementDeadline;

	Tab(std::uint64_t identity, ProcessSupervisor& processes, UiRenderer& renderer, Window& window, bool enabled,
		std::string endpoint)
		: id(identity), workspace(processes, enabled, endpoint), runtime(renderer, workspace.InitialEndpoint(), &window)
	{}

	Tab(std::uint64_t identity, ProcessSupervisor& processes, UiRenderer& renderer, Window& window,
		WorkspacePanels& existing, std::string endpoint)
		: id(identity), workspace(processes, existing), runtime(renderer, endpoint, &window)
	{}
};

ApplicationTabs::ApplicationTabs(ProcessSupervisor& processes, UiRenderer& renderer, Window& window, bool enabled,
								 std::string endpoint, bool showTabs)
	: m_processes(processes), m_renderer(renderer), m_window(window), m_enabled(enabled), m_showTabs(showTabs)
{
	auto& tab = Add(std::move(endpoint));
	m_active = tab.id;
	if (m_showTabs)
		m_watcher.emplace();
}

ApplicationTabs::~ApplicationTabs() = default;

RuntimePanels& ApplicationTabs::ActiveRuntime()
{
	return Find(m_active)->runtime;
}

WorkspacePanels& ApplicationTabs::ActiveWorkspace()
{
	return Find(m_active)->workspace;
}

std::size_t ApplicationTabs::Count() const noexcept
{
	return m_tabs.size();
}

void ApplicationTabs::OpenApplication(RunningApp app)
{
	for (const auto& tab : m_tabs)
		if (!tab->closing && tab->runtime.Endpoint() == app.endpoint && !app.endpoint.empty())
		{
			auto* current = Find(m_active);
			if (current && current->id != tab->id && Idle(*current))
				BeginClose(*current);
			m_select = tab->id;
			return;
		}
	m_open = std::move(app);
	m_openInBackground = false;
}

bool ApplicationTabs::PilotActive() const
{
	return Find(m_active)->runtime.PilotActive();
}

void ApplicationTabs::FocusLost()
{
	Find(m_active)->runtime.FocusLost();
}

void ApplicationTabs::Draw()
{
	ApplyRequests();
	for (const auto& tab : m_tabs)
	{
		const bool active = tab->id == m_active && !tab->closing;
		tab->runtime.Poll(active);
		m_headsetUncertain |= tab->runtime.HeadsetStatus().forcedStop;
		if (!active)
			tab->workspace.Poll(tab->runtime);
	}
	auto& tab = *Find(m_active);
	tab.workspace.BeginFrame(tab.runtime);
	tab.workspace.Draw(tab.runtime);
}

std::vector<VkSemaphore> ApplicationTabs::ReadyWaits() const
{
	std::vector<VkSemaphore> waits;
	for (const auto& tab : m_tabs)
	{
		auto ready = tab->runtime.ReadyWaits();
		waits.insert(waits.end(), ready.begin(), ready.end());
	}
	return waits;
}

void ApplicationTabs::RecordTransfers(VkCommandBuffer command)
{
	for (const auto& tab : m_tabs)
		tab->runtime.RecordTransfers(command);
}

void ApplicationTabs::Submitted(std::uint64_t serial)
{
	for (const auto& tab : m_tabs)
		tab->runtime.Submitted(serial);
}

void ApplicationTabs::Finish()
{
	m_renderer.WaitIdle();
	Find(m_active)->workspace.Finish();
}

std::uint64_t ApplicationTabs::LaunchApplication(const LaunchProfile& profile)
{
	auto* tab = Find(m_active);
	if (!tab || tab->closing || !tab->runtime.Endpoint().empty() ||
		tab->workspace.Process().state != LaunchState::Idle || tab->workspace.HasActiveAutomation() ||
		tab->workspace.HasPendingFiles())
		tab = &Add();
	tab->workspace.LaunchApp(profile);
	if (!tab->workspace.LaunchError().empty())
		throw std::runtime_error(tab->workspace.LaunchError());
	if (tab->workspace.Process().state == LaunchState::Idle)
		throw std::runtime_error("Application launch was not admitted");
	m_select = tab->id;
	return tab->id;
}

void ApplicationTabs::RequestApplicationClose(std::uint64_t id)
{
	auto* tab = Find(id);
	if (!tab || tab->closing)
		throw std::runtime_error("Application is no longer available");
	if (!tab->workspace.RequestApplicationClose(tab->runtime))
		throw std::runtime_error(
			"No running simulator-launched process matches this application, or a close/restart is pending");
}

void ApplicationTabs::RequestApplicationRestart(std::uint64_t id, std::uint64_t expectedProcess)
{
	auto* tab = Find(id);
	if (!tab || tab->closing)
		throw std::runtime_error("Application is no longer available");
	if (!tab->workspace.RequestApplicationRestart(tab->runtime, expectedProcess))
		throw std::runtime_error(tab->workspace.LaunchError());
}

std::vector<ApplicationSnapshot> ApplicationTabs::Applications() const
{
	std::vector<ApplicationSnapshot> result;
	result.reserve(m_tabs.size());
	for (const auto& tab : m_tabs)
	{
		if (tab->closing)
			continue;
		const auto view = tab->runtime.Snapshot();
		const auto launch = tab->workspace.Process();
		if (view.endpoint.empty() && launch.state == LaunchState::Idle)
			continue;
		const bool current = !launch.restartPending && tab->workspace.ConnectionMatchesLaunch(tab->runtime);
		result.push_back(
			{tab->id, current ? view.session : 0, current ? view.generation : 0, current ? view.runtimeProcess : 0,
			 current ? view.epoch : 0, launch, tab->workspace.ApplicationName(tab->runtime),
			 current ? view.endpoint : std::string{}, tab->workspace.LaunchError(), current && view.connected,
			 current && view.sessionRunning, current ? tab->workspace.LaunchControlEndpoint() : std::string{},
			 current && launch.process && launch.process == view.runtimeProcess ? tab->workspace.LaunchDataDirectory()
																				: std::string{}});
	}
	return result;
}

bool ApplicationTabs::RequestShutdown()
{
	return false;
}

#ifdef SIMXR_COMPANION_AUTOMATION
std::uint64_t ApplicationTabs::StartLuau(std::uint64_t id, LuauRunRequest request,
										 const std::shared_ptr<AutomationOwner>& owner)
{
	auto* tab = Find(id);
	if (!tab || tab->closing)
		throw std::runtime_error("Application is no longer available");
	if (tab->workspace.HasActiveAutomation())
		throw std::runtime_error("Stop the current rig before starting another");
	const auto applications = Applications();
	const auto application = std::ranges::find_if(applications, [id](const auto& item) { return item.id == id; });
	if (application == applications.end())
		throw std::runtime_error("Application is no longer available");
	return StartLuauRun(tab->workspace.Rigs(), *application, std::move(request), owner);
}

LuauRunStatus ApplicationTabs::LuauStatus(std::uint64_t id, std::uint64_t run,
										  const std::shared_ptr<AutomationOwner>& owner) const
{
	const auto* tab = Find(id);
	if (!tab || tab->closing)
		throw std::runtime_error("Application is no longer available");
	return ReadLuauRun(tab->workspace.Rigs(), run, owner);
}

void ApplicationTabs::ControlLuau(std::uint64_t id, std::uint64_t run, LuauControl action,
								  const std::shared_ptr<AutomationOwner>& owner)
{
	auto* tab = Find(id);
	if (!tab || tab->closing)
		throw std::runtime_error("Application is no longer available");
	ControlLuauRun(tab->workspace.Rigs(), run, action, owner);
}
#endif

ApplicationTabs::Tab& ApplicationTabs::Add(std::string endpoint)
{
	const auto id = m_nextId++;
	if (m_tabs.empty())
		m_tabs.push_back(std::make_unique<Tab>(id, m_processes, m_renderer, m_window, m_enabled, std::move(endpoint)));
	else
		m_tabs.push_back(std::make_unique<Tab>(id, m_processes, m_renderer, m_window, m_tabs.front()->workspace,
											   std::move(endpoint)));
	auto& tab = *m_tabs.back();
	if (m_showTabs)
	{
		tab.runtime.SetHeadsetAdmission([this, id] {
			if (m_headsetUncertain)
				throw std::runtime_error(
					"The headset did not stop cleanly. Restart the companion before reconnecting.");
			for (const auto& other : m_tabs)
			{
				if (other->id == id)
					continue;
				const auto phase = other->runtime.HeadsetStatus().phase;
				if (phase != HeadsetHostPhase::Idle && phase != HeadsetHostPhase::Exited &&
					phase != HeadsetHostPhase::Failed)
					return false;
			}
			return true;
		});
		tab.workspace.SetTabBar([this] { DrawBar(); });
		tab.workspace.SetEndpointAdmission([this, id](const std::string& endpoint) {
			return std::ranges::none_of(m_tabs, [&](const auto& other) {
				return other->id != id && !other->closing && other->runtime.Endpoint() == endpoint;
			});
		});
		tab.runtime.SetEndpointOpenHandler([this, id](const std::string& endpoint) {
			if (endpoint.empty())
				return false;
			for (const auto& other : m_tabs)
				if (!other->closing && other->runtime.Endpoint() == endpoint)
				{
					OpenApplication(RunningApp{endpoint});
					return true;
				}
			const auto* current = Find(id);
			if (current &&
				(!current->runtime.Endpoint().empty() || current->workspace.Process().state == LaunchState::Starting ||
				 current->workspace.Process().state == LaunchState::Running))
			{
				m_open = RunningApp{endpoint};
				return true;
			}
			return false;
		});
		tab.workspace.SetOpenHandlers(
			[this, id](const RunningApp& app) {
				for (const auto& other : m_tabs)
					if (!other->closing && other->runtime.Endpoint() == app.endpoint)
					{
						OpenApplication(app);
						return true;
					}
				auto* current = Find(id);
				if (current && (!current->runtime.Endpoint().empty() ||
								current->workspace.Process().state == LaunchState::Starting ||
								current->workspace.Process().state == LaunchState::Running))
				{
					OpenApplication(app);
					return true;
				}
				if (current)
					current->process = app.process;
				return false;
			},
			[this, id](const LaunchProfile& profile) {
				const auto* current = Find(id);
				if (current && (!current->runtime.Endpoint().empty() ||
								current->workspace.Process().state == LaunchState::Starting ||
								current->workspace.Process().state == LaunchState::Running))
				{
					m_open = profile;
					return true;
				}
				return false;
			});
	}
	return tab;
}

void ApplicationTabs::OpenAppeared()
{
	if (!m_watcher)
		return;
	for (auto& app : m_watcher->TakeAppeared())
		m_appeared.push_back({std::move(app), std::chrono::steady_clock::now()});
	if (!Find(m_active)->workspace.OpensNewApps())
		m_appeared.clear();
	if (m_appeared.empty() || m_add || m_open)
		return;
	const auto& [app, since] = m_appeared.front();
	const auto sameProcess = [&](const Tab& tab) {
		return app.process && (tab.process == app.process || tab.workspace.Process().process == app.process);
	};
	const auto owned = [&](const Tab& tab) {
		return !tab.closing && !Disconnected(tab) && (tab.runtime.Endpoint() == app.endpoint || sameProcess(tab));
	};
	// A launch from this companion connects its own tab once the runtime answers.
	const bool launching = std::ranges::any_of(m_tabs, [](const auto& tab) {
		const auto state = tab->workspace.Process().state;
		return !tab->closing && tab->runtime.Endpoint().empty() &&
			   (state == LaunchState::Starting || state == LaunchState::Running);
	});
	if (launching && std::chrono::steady_clock::now() - since < std::chrono::seconds(10))
		return;
	if (std::ranges::none_of(m_tabs, [&](const auto& tab) { return owned(*tab); }))
	{
		// An app that starts again returns to the tab it left; otherwise an unused active tab takes it.
		Tab* target = nullptr;
		for (const auto& tab : m_tabs)
			if (Disconnected(*tab) && (sameProcess(*tab) || tab->workspace.ApplicationName(tab->runtime) == app.name))
			{
				target = tab.get();
				break;
			}
		if (auto& active = *Find(m_active); !target && (Idle(active) || Disconnected(active)))
			target = &active;
		if (target)
		{
			target->process = app.process;
			target->workspace.ConnectApp(target->runtime, app);
		}
		else
		{
			m_open = app;
			m_openInBackground = true;
		}
	}
	m_appeared.erase(m_appeared.begin());
}

bool ApplicationTabs::Disconnected(const Tab& tab) const
{
	const auto view = tab.runtime.Snapshot();
	return !tab.closing && !tab.runtime.Endpoint().empty() && view.epoch && !view.connected && view.error.empty() &&
		   view.connectionState == "Disconnected" && !view.recording && !tab.runtime.Exporting() &&
		   !tab.workspace.HasActiveAutomation() && !tab.workspace.HasPendingFiles();
}

bool ApplicationTabs::Idle(const Tab& tab) const
{
	const auto host = tab.runtime.HeadsetStatus();
	return !tab.closing && tab.runtime.Endpoint().empty() && tab.workspace.Process().state == LaunchState::Idle &&
		   !tab.runtime.Snapshot().recording && !tab.runtime.Exporting() && !tab.workspace.HasActiveAutomation() &&
		   !tab.workspace.HasPendingFiles() &&
		   (host.phase == HeadsetHostPhase::Idle || host.phase == HeadsetHostPhase::Exited);
}

ApplicationTabs::Tab* ApplicationTabs::Find(std::uint64_t id) const
{
	for (const auto& tab : m_tabs)
		if (tab->id == id)
			return tab.get();
	return nullptr;
}

void ApplicationTabs::ApplyRequests()
{
	OpenAppeared();
	if (m_add || m_open)
	{
		auto& tab = Add();
		if (!std::exchange(m_openInBackground, false))
			m_select = tab.id;
		if (m_open)
		{
			if (const auto* app = std::get_if<RunningApp>(&*m_open))
			{
				tab.process = app->process;
				tab.workspace.ConnectApp(tab.runtime, *app);
			}
			else
				tab.workspace.LaunchApp(std::get<LaunchProfile>(*m_open));
			m_open.reset();
		}
		else
			tab.workspace.OpenAppPicker();
		m_add = false;
	}
	if (m_select && *m_select != m_active)
	{
		if (auto* next = Find(*m_select); next && !next->closing)
		{
			Find(m_active)->runtime.Deactivate();
			m_active = next->id;
			m_forceSelection = true;
		}
	}
	m_select.reset();
	if (std::ranges::all_of(m_tabs, [](const auto& tab) { return tab->closing; }))
	{
		auto& replacement = Add();
		m_active = replacement.id;
		m_forceSelection = true;
	}
	else if (Find(m_active)->closing)
	{
		auto replacement = std::ranges::find_if(m_tabs, [](const auto& candidate) {
			return !candidate->closing && !candidate->runtime.Endpoint().empty();
		});
		if (replacement == m_tabs.end())
			replacement = std::ranges::find_if(m_tabs, [](const auto& candidate) { return !candidate->closing; });
		m_active = (*replacement)->id;
		m_forceSelection = true;
	}
	for (auto it = m_tabs.begin(); it != m_tabs.end();)
	{
		auto& tab = **it;
		const auto host = tab.runtime.HeadsetStatus();
		const bool hostStopped = host.phase == HeadsetHostPhase::Idle || host.phase == HeadsetHostPhase::Exited ||
								 host.phase == HeadsetHostPhase::Failed;
		if (!tab.closing || tab.runtime.Snapshot().recording || tab.runtime.Exporting() ||
			!tab.workspace.ClosingWorkDone() || !hostStopped)
		{
			++it;
			continue;
		}
		if (!tab.retiring)
		{
			tab.runtime.BeginRetirement();
			tab.retiring = true;
			tab.retirementDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		}
		if (!tab.runtime.RetirementComplete())
		{
			if (std::chrono::steady_clock::now() < tab.retirementDeadline)
			{
				++it;
				continue;
			}
			tab.runtime.PreserveUnfinished();
			Find(m_active)->runtime.Notify("Cleanup incomplete", "Unfinished resources remain reserved until exit.");
		}
		if (tab.id == m_active)
		{
			auto* replacement = static_cast<Tab*>(nullptr);
			for (const auto& candidate : m_tabs)
				if (candidate->id != tab.id && !candidate->closing)
				{
					replacement = candidate.get();
					break;
				}
			if (!replacement)
				break;
			m_active = replacement->id;
			m_forceSelection = true;
		}
		it = m_tabs.erase(it);
	}
}

void ApplicationTabs::DrawBar()
{
	bool requestConfirmation = false;
	const float scale = ImGui::GetFontSize() / 17, iconSize = ImGui::GetFrameHeight() * .6f;
	// A tab's text sits on the bar icons' centre line while its body reaches the bottom of the bar. ImGui sizes a tab
	// around its text, so its own background is cleared and each body is drawn on a channel beneath the text.
	const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight();
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10 * scale, ImGui::GetStyle().FramePadding.y});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, {4 * scale, ImGui::GetStyle().ItemInnerSpacing.y});
	ImGui::PushStyleVar(ImGuiStyleVar_TabBarBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_TabBarOverlineSize, 0.0f);
	for (const auto color :
		 {ImGuiCol_Tab, ImGuiCol_TabHovered, ImGuiCol_TabSelected, ImGuiCol_TabDimmed, ImGuiCol_TabDimmedSelected})
		ImGui::PushStyleColor(color, ImVec4{});
	auto* draw = ImGui::GetWindowDrawList();
	ImDrawListSplitter layers;
	layers.Split(draw, 2);
	layers.SetCurrentChannel(draw, 1);
	const auto pill = [&](bool selected) {
		const auto minimum = ImGui::GetItemRectMin();
		const ImVec2 maximum{ImGui::GetItemRectMax().x, bottom};
		const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
		const ImU32 fill = selected	 ? theme::Mix(theme::SurfaceRaised, theme::Accent, hovered ? .3f : .22f)
						   : hovered ? theme::Mix(theme::SurfaceRaised, theme::Text, .1f)
									 : theme::Surface;
		layers.SetCurrentChannel(draw, 0);
		draw->AddRectFilled(minimum, maximum, fill, 7 * scale, ImDrawFlags_RoundCornersTop);
		if (selected)
			draw->AddRectFilled({minimum.x, maximum.y - 2 * scale}, maximum, theme::Accent);
		layers.SetCurrentChannel(draw, 1);
	};
	if (ImGui::BeginTabBar("Applications", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll))
	{
		for (const auto& tab : m_tabs)
		{
			const auto view = tab->runtime.Snapshot();
			const auto name = tab->workspace.ApplicationName(tab->runtime);
			std::string title;
			for (const char character : name)
				if (character == '#')
					title += "＃";
				else
					title += character;
			const auto launched = tab->workspace.Process();
			const auto process = launched.process ? launched.process : tab->process;
			if (process)
				title += " (" + std::to_string(process) + ")";
			// A status icon is drawn over spaces reserved at the start of the title.
			std::optional<std::pair<Icon, ImU32>> status;
			if (!tab->closing && !view.error.empty())
				status = {Icon::Warning, theme::Record};
			else if (!tab->closing && view.recording)
				status = {Icon::Record, theme::Record};
			if (status)
				title.insert(0,
							 static_cast<std::size_t>(std::ceil((iconSize + ImGui::GetStyle().ItemInnerSpacing.x) /
																ImGui::CalcTextSize(" ").x)),
							 ' ');
			if (tab->closing)
				title += " (closing)";
			else if (!status)
			{
				if (view.manual)
					title += " (Paused)";
				else if (!view.connected &&
						 (launched.state == LaunchState::Starting || view.connectionState == "Connecting"))
					title += " (Connecting)";
				else if (!view.connected && !tab->runtime.Endpoint().empty())
					title += " (Disconnected)";
			}
			title += "###application-" + std::to_string(tab->id);
			bool open = true;
			const auto flags = m_forceSelection && tab->id == m_active ? ImGuiTabItemFlags_SetSelected : 0;
			ImGui::PushStyleColor(ImGuiCol_Text, tab->id == m_active ? theme::Text : theme::TextDim);
			const bool selected = ImGui::BeginTabItem(title.c_str(), &open, flags);
			ImGui::PopStyleColor();
			pill(tab->id == m_active);
			if (status)
			{
				const auto minimum = ImGui::GetItemRectMin(), maximum = ImGui::GetItemRectMax();
				DrawIcon(ImGui::GetWindowDrawList(), status->first,
						 {minimum.x + ImGui::GetStyle().FramePadding.x + iconSize / 2, (minimum.y + maximum.y) / 2},
						 iconSize, status->second);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s\n%s%s%s%s%s", name.c_str(), view.connectionState.c_str(),
								  view.error.empty() ? "" : "\n", view.error.c_str(),
								  tab->runtime.Endpoint().empty() ? "" : "\n", tab->runtime.Endpoint().c_str());
			if (selected)
			{
				if (!m_forceSelection && !tab->closing && tab->id != m_active)
					m_select = tab->id;
				ImGui::EndTabItem();
			}
			if (!open && !tab->closing)
			{
				m_forceSelection = true;
				const auto host = tab->runtime.HeadsetStatus();
				if (view.recording || tab->workspace.HasActiveAutomation() ||
					(host.phase != HeadsetHostPhase::Idle && host.phase != HeadsetHostPhase::Exited &&
					 host.phase != HeadsetHostPhase::Failed))
				{
					m_confirmClose = tab->id;
					requestConfirmation = true;
				}
				else
					BeginClose(*tab);
			}
		}
		if (!m_confirmClose)
			m_forceSelection = false;
		if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing))
			m_add = true;
		pill(false);
		ImGui::SetItemTooltip("Open another application");
		ImGui::EndTabBar();
	}
	layers.Merge(draw);
	ImGui::PopStyleColor(5);
	ImGui::PopStyleVar(4);
	if (requestConfirmation)
		ImGui::OpenPopup("Close application tab");
	if (ImGui::BeginPopupModal("Close application tab", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextUnformatted(
			"Stop this tab's recording, replay and headset connection?\nThe application keeps running.");
		if (ImGui::Button("Stop and close"))
		{
			if (m_confirmClose)
				if (auto* tab = Find(*m_confirmClose))
					BeginClose(*tab);
			m_confirmClose.reset();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
		{
			m_confirmClose.reset();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

void ApplicationTabs::BeginClose(Tab& tab)
{
	if (tab.id == m_active)
		tab.runtime.Deactivate();
	tab.workspace.BeginClose();
	tab.runtime.FinishReplayPreparation();
	tab.runtime.StopRecording();
	tab.runtime.DisconnectHeadset();
	tab.closing = true;
}
} // namespace simxr::app
