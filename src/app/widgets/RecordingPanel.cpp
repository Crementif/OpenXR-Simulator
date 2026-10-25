#include "app/widgets/RecordingPanel.h"
#include "Pch.h"
#include "app/widgets/Theme.h"
#include "app/services/CompanionSettings.h"
#include "app/services/Utf8Path.h"

#include <ctime>
#include <imgui.h>
#include <stdexcept>

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
	m_name = {};
	m_error.clear();
}
RecordingSnapshot RecordingPanel::Snapshot() const
{
	auto state = m_controller.Snapshot();
	if (!m_error.empty())
		state.error = m_error;
	return state;
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
	if (supported && !state.busy && state.error.empty() && !m_startRequested && !m_stopRequested && !m_popupTarget &&
		now >= m_nextRead)
	{
		Request(RecordingAction::Read);
		m_nextRead = now + std::chrono::seconds(1);
	}
}
bool RecordingPanel::Request(RecordingAction action, std::string path)
{
	try
	{
		if (!m_controller.Request(action, 0, std::move(path)))
			return false;
		m_error.clear();
		return true;
	}
	catch (const std::exception& error)
	{
		m_error = error.what();
		return false;
	}
}
bool RecordingPanel::Start()
{
	if (!Request(RecordingAction::Start))
		return false;
	m_discarded = false;
	return true;
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
		m_saving = Request(RecordingAction::Save, PathText(m_directory / Utf8Path(name + ".luau")));
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
	if (!state.busy && std::exchange(m_stopRequested, false) && active && Request(RecordingAction::Stop))
	{
		const auto now = std::time(nullptr);
		if (const auto local = std::localtime(&now))
			std::strftime(m_name.data(), m_name.size(), "Take %Y-%m-%d %H-%M-%S", local);
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
		ImGui::BeginDisabled(state.busy || !supported || active);
		if (ImGui::Button("Record new take") && Start())
		{
			m_popupTarget.reset();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Keep it"))
		{
			m_popupTarget.reset();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	if (!ImGui::BeginPopup("Save take"))
	{
		if (!ImGui::IsPopupOpen("Replace unsaved take?"))
			m_popupTarget.reset();
		return;
	}
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
	bool exists = false;
	std::string pathError;
	try
	{
		std::error_code error;
		exists = !m_directory.empty() &&
				 std::filesystem::exists(m_directory / Utf8Path(std::string(m_name.data()) + ".luau"), error);
		if (error)
			pathError = error.message();
	}
	catch (const std::exception& error)
	{
		pathError = error.what();
	}
	const bool ready = supported && state.page && !active && !state.busy && !state.page->overflow && pathError.empty();
	if (exists)
		ImGui::TextColored(theme::Vec(theme::Warn), "Replaces the take with this name");
	if (state.page && state.page->overflow)
		ImGui::TextColored(theme::Vec(theme::Record), "The recording is full and cannot be saved.");
	ImGui::BeginDisabled(!ready || m_saving);
	if (ImGui::Button(exists ? "Replace take" : "Save") || (entered && !exists && ready && !m_saving))
		Save();
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(m_saving || state.busy || active);
	if (ImGui::Button("Discard"))
	{
		m_discarded = true;
		m_popupTarget.reset();
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	if (m_saving || state.busy)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("%s", m_saving ? "Saving..." : "Stopping...");
	}
	if (m_saved)
	{
		m_popupTarget.reset();
		ImGui::CloseCurrentPopup();
	}
	for (const std::string* message : {&std::as_const(pathError), &state.error})
		if (!message->empty())
		{
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 260 * unit);
			ImGui::TextColored(theme::Vec(theme::Record), "%s", message->c_str());
			ImGui::PopTextWrapPos();
		}
	ImGui::EndPopup();
}
} // namespace simxr::app
