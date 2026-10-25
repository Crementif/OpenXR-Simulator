#include "app/widgets/HapticEvents.h"
#include "Pch.h"
#include "app/services/HapticFeedback.h"

#include <imgui.h>

namespace simxr::app
{
static const char* StatusName(HapticStatus status)
{
	switch (status)
	{
	case HapticStatus::Active:
		return "Active";
	case HapticStatus::Expired:
		return "Ended";
	case HapticStatus::Stopped:
		return "Stopped";
	case HapticStatus::Replaced:
		return "Replaced";
	case HapticStatus::FocusLost:
		return "Focus lost";
	}
	return "Unknown";
}
void DrawHapticEvents(const HapticFeedback& feedback, std::uint32_t hand, bool supported)
{
	if (!ImGui::CollapsingHeader("Haptics"))
		return;
	ImGui::TextDisabled("Live application output");
	ImGui::SetItemTooltip("Left hand drives the low-frequency motor. Right hand drives the high-frequency motor. "
						  "Each event is limited to two seconds. Rumble stops when this window loses focus.");
	if (!supported)
	{
		ImGui::TextDisabled("Live haptics unavailable");
		return;
	}
	if (!feedback.Fresh(HapticFeedback::Clock::now()))
		ImGui::TextDisabled("Waiting for live output");
	if (feedback.Recent().overflow)
		ImGui::TextDisabled("Older events omitted");
	std::size_t count = 0;
	if (ImGui::BeginTable("Haptic events", 3, ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthStretch, 1);
		ImGui::TableSetupColumn("Frequency", ImGuiTableColumnFlags_WidthStretch, 1);
		ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 1);
		ImGui::TableHeadersRow();
		for (auto it = feedback.Recent().events.rbegin(); it != feedback.Recent().events.rend() && count < 5; ++it)
		{
			if (it->device.kind != InputDeviceKind::Controller || it->device.value != hand)
				continue;
			++count;
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%.0f%%", it->amplitude * 100);
			ImGui::SetItemTooltip("Event %llu", static_cast<unsigned long long>(it->sequence));
			ImGui::TableNextColumn();
			if (it->frequency == 0)
				ImGui::TextUnformatted("Default");
			else
				ImGui::Text("%.0f Hz", it->frequency);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(StatusName(it->status));
		}
		ImGui::EndTable();
	}
	if (!count)
		ImGui::TextDisabled("No recent events");
}
} // namespace simxr::app
