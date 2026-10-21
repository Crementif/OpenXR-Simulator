#include "app/RecordingPanel.h"

#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <utility>

namespace simxr::app
{
RecordingPanel::RecordingPanel()
{
	std::snprintf(m_path.data(), m_path.size(), "%s", "recording.luau");
}
void RecordingPanel::SetTarget(RecordingTarget target)
{
	if (target == m_target)
		return;
	m_controller.SetTarget(target);
	m_target = std::move(target);
	m_offset = 0;
}
std::optional<automation::SavedRecording> RecordingPanel::Draw(bool supported, bool replayAvailable)
{
	std::optional<automation::SavedRecording> replay;
	ImGui::Begin("Recording");
	const auto snapshot = m_controller.Snapshot();
	ImGui::BeginDisabled(!supported || snapshot.busy);
	if (ImGui::Button("Start recording"))
		m_controller.Request(RecordingAction::Start);
	ImGui::SameLine();
	if (ImGui::Button("Stop recording"))
		m_controller.Request(RecordingAction::Stop);
	if (ImGui::Button("Read recording"))
		m_controller.Request(RecordingAction::Read, m_offset);
	ImGui::SetNextItemWidth(160);
	ImGui::InputScalar("Transaction offset", ImGuiDataType_U64, &m_offset);
	m_offset = std::min<std::uint64_t>(1024, m_offset);
	ImGui::BeginDisabled(m_offset == 0);
	if (ImGui::Button("Previous page"))
	{
		m_offset = m_offset > 16 ? m_offset - 16 : 0;
		m_controller.Request(RecordingAction::Read, m_offset);
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Next page"))
	{
		m_offset = std::min<std::uint64_t>(snapshot.page ? snapshot.page->total : 1024, m_offset + 16);
		m_controller.Request(RecordingAction::Read, m_offset);
	}
	ImGui::TextUnformatted("Editable rig destination");
	ImGui::InputText("##Recording destination", m_path.data(), m_path.size());
	ImGui::BeginDisabled(snapshot.page && (snapshot.page->active || snapshot.page->overflow));
	if (ImGui::Button("Save editable rig"))
		m_controller.Request(RecordingAction::Save, 0, m_path.data());
	ImGui::EndDisabled();
	ImGui::BeginDisabled(!replayAvailable || snapshot.savedPath.empty());
	if (ImGui::Button("Replay saved rig"))
		replay = automation::SavedRecording{snapshot.savedPath, snapshot.savedSeed};
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	if (snapshot.busy)
		ImGui::TextUnformatted("Recording request in progress");
	if (!snapshot.savedPath.empty())
		ImGui::TextWrapped("Saved: %s", snapshot.savedPath.c_str());
	if (!snapshot.error.empty())
		ImGui::TextWrapped("%s", snapshot.error.c_str());
	ImGui::TextWrapped("Saving collects the complete stopped recording on its worker. Edit the saved command tables "
					   "before replaying them in Rigs. Ownership conflicts and recording overflow remain explicit.");
	if (const auto& page = snapshot.page)
	{
		ImGui::Text("%s; seed %llu; %llu transactions", page->active ? "Recording" : "Stopped",
					static_cast<unsigned long long>(page->seed), static_cast<unsigned long long>(page->total));
		if (page->overflow)
			ImGui::TextUnformatted("Recording overflowed; a complete replay cannot be saved");
		int index = 0;
		for (const auto& transaction : page->transactions)
		{
			ImGui::PushID(index++);
			const auto label = "Transaction " + std::to_string(transaction.id) + " / " + transaction.phase +
							   " / frame " + std::to_string(transaction.frame.value);
			if (ImGui::TreeNode(label.c_str()))
			{
				for (const auto& command : transaction.commands)
					ImGui::TextWrapped("%s", automation::EditableCommand(command).c_str());
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
	}
	ImGui::End();
	return replay;
}
} // namespace simxr::app
