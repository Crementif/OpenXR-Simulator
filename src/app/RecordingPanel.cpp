#include "app/RecordingPanel.h"
#include "app/Theme.h"
#include "app/CompanionSettings.h"
#include "app/Utf8Path.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <imgui.h>
#include <utility>

namespace simxr::app
{
namespace
{
std::string PathText(const std::filesystem::path& path)
{
	const auto text = path.u8string();
	return {text.begin(), text.end()};
}
} // namespace
RecordingPanel::RecordingPanel()
{
	const auto now = std::time(nullptr);
	std::strftime(m_name.data(), m_name.size(), "Take %Y-%m-%d %H-%M-%S", std::localtime(&now));
	try
	{
		m_directory = CompanionSettingsPath().parent_path() / "recordings";
		RefreshFiles();
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
	m_offset = 0;
	m_nextRead = {};
	m_saving = false;
}
RecordingSnapshot RecordingPanel::Snapshot() const
{
	return m_controller.Snapshot();
}
void RecordingPanel::Stop()
{
	m_controller.Request(RecordingAction::Stop);
}
void RecordingPanel::Poll(bool supported)
{
	const auto state = Snapshot();
	if (m_saving && !state.busy)
	{
		m_saving = false;
		if (state.error.empty())
		{
			RefreshFiles();
			SelectFile(state.savedPath);
		}
	}
	const auto now = std::chrono::steady_clock::now();
	if (supported && !state.busy && state.error.empty() && now >= m_nextRead)
	{
		m_controller.Request(RecordingAction::Read, m_offset);
		m_nextRead = now + std::chrono::seconds(1);
	}
}
void RecordingPanel::RefreshFiles()
{
	m_files.clear();
	if (m_directory.empty())
		return;
	std::error_code error;
	if (!std::filesystem::exists(m_directory, error))
		return;
	for (std::filesystem::directory_iterator it(m_directory, error), end; !error && it != end; it.increment(error))
	{
		if (it->is_regular_file(error) && it->path().extension() == ".luau")
			m_files.push_back(PathText(it->path()));
		if (m_files.size() >= 128)
			break;
	}
	std::sort(m_files.begin(), m_files.end(), std::greater<>());
	if (error)
		m_error = "Cannot read the recordings folder: " + error.message();
}
void RecordingPanel::SelectFile(const std::string& path)
{
	try
	{
		m_selected = OpenSavedRecording(path);
		m_error.clear();
	}
	catch (const std::exception& error)
	{
		m_selected.reset();
		m_error = error.what();
	}
}
void RecordingPanel::Save()
{
	try
	{
		const std::string name = m_name.data();
		if (m_directory.empty())
			throw std::runtime_error("The recordings folder is unavailable");
		if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos)
			throw std::runtime_error("Give the recording a name without folder separators or a colon");
		std::filesystem::create_directories(m_directory);
		m_saving = m_controller.Request(RecordingAction::Save, 0, PathText(m_directory / Utf8Path(name + ".luau")));
		m_error.clear();
	}
	catch (const std::exception& error)
	{
		m_error = error.what();
	}
}
std::optional<automation::SavedRecording> RecordingPanel::Draw(bool supported, bool replayAvailable, bool replayActive)
{
	std::optional<automation::SavedRecording> replay;
	ImGui::SeparatorText("Takes");
	const auto state = Snapshot();
	const bool active = state.page && state.page->active;
	ImGui::SeparatorText("Capture input");
	if (!supported)
		ImGui::TextUnformatted("Recording unavailable");
	ImGui::BeginDisabled(!supported || state.busy || replayActive);
	const bool clicked = ImGui::Button(active ? "Stop recording" : "Start recording", {-1, 0});
	const bool requested = std::exchange(m_startRequested, false);
	if ((clicked || (requested && !active)) && supported && !state.busy && !replayActive)
	{
		if (active)
			Stop();
		else if (state.page && state.page->total && state.savedPath.empty())
			ImGui::OpenPopup("Replace unsaved take?");
		else
		{
			m_controller.Request(RecordingAction::Start);
			m_offset = 0;
		}
	}
	if (ImGui::BeginPopup("Replace unsaved take?"))
	{
		ImGui::TextUnformatted(
			"Starting a new take replaces the unsaved input.\nSave it first if you want to keep it.");
		if (ImGui::Button("Start new take"))
		{
			m_controller.Request(RecordingAction::Start);
			m_offset = 0;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Keep current take"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	ImGui::EndDisabled();
	if (state.page)
	{
		ImGui::TextColored(active ? theme::Vec(theme::Record) : ImGui::GetStyleColorVec4(ImGuiCol_Text),
						   "%s / %llu input changes", active ? "Recording" : "Stopped",
						   static_cast<unsigned long long>(state.page->total));
		if (state.page->overflow)
			ImGui::TextUnformatted("Recording full");
	}
	ImGui::TextUnformatted("Take name");
	ImGui::SetNextItemWidth(-1);
	ImGui::InputText("##Take name", m_name.data(), m_name.size());
	ImGui::BeginDisabled(!supported || state.busy || !state.page || active || (state.page && state.page->overflow));
	if (ImGui::Button("Save take", {-1, 0}))
	{
		std::error_code error;
		if (std::filesystem::exists(m_directory / Utf8Path(std::string(m_name.data()) + ".luau"), error))
			ImGui::OpenPopup("Replace saved take?");
		else
			Save();
	}
	if (ImGui::BeginPopup("Replace saved take?"))
	{
		ImGui::TextColored(theme::Vec(theme::Record), "Name already used");
		if (ImGui::Button("Replace file"))
		{
			Save();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	ImGui::EndDisabled();
	if (state.busy)
		ImGui::TextDisabled("%s", m_saving ? "Saving complete take..." : "Updating recording...");
	if (!state.savedPath.empty())
		ImGui::TextWrapped("Saved: %s", PathText(Utf8Path(state.savedPath).filename()).c_str());
	if (!state.error.empty())
	{
		ImGui::TextWrapped("%s", state.error.c_str());
		ImGui::BeginDisabled(!supported || state.busy);
		if (ImGui::Button("Refresh recording state"))
			m_controller.Request(RecordingAction::Read, m_offset);
		ImGui::EndDisabled();
	}

	ImGui::SeparatorText("Saved takes");
	if (m_files.empty())
		ImGui::TextUnformatted("No takes");
	else if (ImGui::BeginListBox("##Saved takes", {-1, ImGui::GetTextLineHeightWithSpacing() * 4.5f}))
	{
		for (const auto& path : m_files)
		{
			ImGui::PushID(path.c_str());
			if (ImGui::Selectable(PathText(Utf8Path(path).stem()).c_str(), m_selected && m_selected->path == path))
				SelectFile(path);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", path.c_str());
			ImGui::PopID();
		}
		ImGui::EndListBox();
	}
	ImGui::BeginDisabled(!replayAvailable || !m_selected || active);
	if (ImGui::Button("Replay selected take", {-1, 0}))
	{
		SelectFile(m_selected->path);
		if (m_selected)
			replay = m_selected;
	}
	ImGui::EndDisabled();
	if (ImGui::CollapsingHeader("Open file and script details"))
	{
		ImGui::TextWrapped("Folder: %s", PathText(m_directory).c_str());
		if (ImGui::Button("Refresh saved takes"))
			RefreshFiles();
		ImGui::TextUnformatted("Saved recording file (.luau)");
		ImGui::SetNextItemWidth(-1);
		ImGui::InputText("##Recording file", m_path.data(), m_path.size());
		if (ImGui::Button("Open recording"))
			SelectFile(m_path.data());
		if (m_selected)
			ImGui::TextWrapped("Selected: %s\nSeed: %llu", m_selected->path.c_str(),
							   static_cast<unsigned long long>(m_selected->seed));
	}
	if (!m_error.empty())
		ImGui::TextWrapped("%s", m_error.c_str());
	if (state.page && ImGui::CollapsingHeader("Recorded input details"))
	{
		const auto total = state.page->total;
		ImGui::Text("Changes %llu–%llu of %llu", static_cast<unsigned long long>(total ? state.offset + 1 : 0),
					static_cast<unsigned long long>(std::min(total, state.offset + state.page->transactions.size())),
					static_cast<unsigned long long>(total));
		ImGui::BeginDisabled(!supported || state.busy || state.offset == 0);
		if (ImGui::Button("Previous"))
		{
			m_offset = state.offset > 16 ? state.offset - 16 : 0;
			m_controller.Request(RecordingAction::Read, m_offset);
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!supported || state.busy || state.offset + 16 >= total);
		if (ImGui::Button("Next"))
		{
			m_offset = state.offset + 16;
			m_controller.Request(RecordingAction::Read, m_offset);
		}
		ImGui::EndDisabled();
		for (const auto& transaction : state.page->transactions)
		{
			ImGui::PushID(static_cast<int>(transaction.id));
			const auto label = "Frame " + std::to_string(transaction.frame.value) + " / " + transaction.phase;
			if (ImGui::TreeNode(label.c_str()))
			{
				for (const auto& command : transaction.commands)
					ImGui::TextWrapped("%s", automation::EditableCommand(command).c_str());
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
	}

	return replay;
}
} // namespace simxr::app
