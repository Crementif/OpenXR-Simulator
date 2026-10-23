#include "app/RecordingPanel.h"
#include "app/services/CompanionSettings.h"
#include "app/Theme.h"
#include "app/services/Utf8Path.h"

#include <cstdio>
#include <ctime>
#include <imgui.h>
#include <utility>

namespace simxr::app
{
RecordingPanel::RecordingPanel()
{
	try
	{
		m_directory = CompanionSettingsPath().parent_path() / "recordings";
	}
	catch (const std::exception& error)
	{
		m_error = error.what();
	}
}
void RecordingPanel::SetTarget(RecordingTarget target)
{
	if (target == m_target)
		return;
	m_controller.SetTarget(target);
	m_target = std::move(target);
	m_nextRead = {};
	m_saving = false;
	m_startRequested = m_stopRequested = m_discarded = false;
	m_saved.reset();
	m_popupTarget.reset();
	m_error.clear();
}
RecordingSnapshot RecordingPanel::Snapshot() const
{
	return m_controller.Snapshot();
}
void RecordingPanel::Poll(bool supported)
{
	const auto state = Snapshot();
	if (m_saving && !state.busy)
	{
		m_saving = false;
		if (state.error.empty() && !state.savedPath.empty())
			m_saved = state.savedPath;
	}
	const auto now = std::chrono::steady_clock::now();
	if (supported && !state.busy && state.error.empty() && now >= m_nextRead)
	{
		m_controller.Request(RecordingAction::Read);
		m_nextRead = now + std::chrono::seconds(1);
	}
}
void RecordingPanel::Start()
{
	m_controller.Request(RecordingAction::Start);
	m_discarded = false;
	m_error.clear();
}
void RecordingPanel::Save()
{
	try
	{
		const std::string name = m_name.data();
		if (m_directory.empty())
			throw std::runtime_error("The recordings folder is unavailable");
		if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos)
			throw std::runtime_error("Name without / \\ or :");
		std::filesystem::create_directories(m_directory);
		m_saving = m_controller.Request(RecordingAction::Save, 0, PathText(m_directory / Utf8Path(name + ".luau")));
		m_error.clear();
	}
	catch (const std::exception& error)
	{
		m_error = error.what();
	}
}
void RecordingPanel::DrawPopovers(bool supported)
{
	const auto state = Snapshot();
	const bool active = state.page && state.page->active;
	// A request waits out a busy read; the controller takes one action at a time.
	if (!state.busy && std::exchange(m_startRequested, false) && supported && !active)
	{
		if (state.page && state.page->total && state.savedPath.empty() && !m_discarded)
		{
			m_popupTarget = m_target;
			ImGui::OpenPopup("Replace unsaved take?");
		}
		else
			Start();
	}
	if (!state.busy && std::exchange(m_stopRequested, false) && active && m_controller.Request(RecordingAction::Stop))
	{
		const auto now = std::time(nullptr);
		std::strftime(m_name.data(), m_name.size(), "Take %Y-%m-%d %H-%M-%S", std::localtime(&now));
		m_error.clear();
		m_popupTarget = m_target;
		ImGui::OpenPopup("Save take");
	}
	if (ImGui::BeginPopup("Replace unsaved take?"))
	{
		if (m_popupTarget != m_target)
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		ImGui::TextUnformatted("The last take is not saved");
		if (ImGui::Button("Record new take"))
		{
			Start();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Keep it"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (!ImGui::BeginPopup("Save take"))
		return;
	if (m_popupTarget != m_target)
	{
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	const float unit = ImGui::GetFontSize() / 17;
	ImGui::TextUnformatted("Save take");
	if (state.page)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("%llu input changes", static_cast<unsigned long long>(state.page->total));
	}
	ImGui::SetNextItemWidth(260 * unit);
	if (ImGui::IsWindowAppearing())
		ImGui::SetKeyboardFocusHere();
	const bool entered =
		ImGui::InputText("##Take name", m_name.data(), m_name.size(), ImGuiInputTextFlags_EnterReturnsTrue);
	std::error_code error;
	const bool exists =
		!m_directory.empty() && std::filesystem::exists(m_directory / Utf8Path(std::string(m_name.data()) + ".luau"), error);
	const bool ready = supported && state.page && !active && !state.busy && !state.page->overflow;
	if (exists)
		ImGui::TextColored(theme::Vec(theme::Warn), "Replaces the take with this name");
	if (state.page && state.page->overflow)
		ImGui::TextColored(theme::Vec(theme::Record), "Recording full; cannot save");
	ImGui::BeginDisabled(!ready || m_saving);
	if (ImGui::Button("Save") || (entered && ready && !m_saving))
		Save();
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Discard"))
	{
		m_discarded = true;
		ImGui::CloseCurrentPopup();
	}
	if (m_saving || state.busy)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("%s", m_saving ? "Saving..." : "Stopping...");
	}
	if (m_saved)
		ImGui::CloseCurrentPopup();
	for (const std::string* message : {&std::as_const(m_error), &state.error})
		if (!message->empty())
		{
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 260 * unit);
			ImGui::TextColored(theme::Vec(theme::Record), "%s", message->c_str());
			ImGui::PopTextWrapPos();
		}
	ImGui::EndPopup();
}
} // namespace simxr::app
