#include "app/widgets/LiveField.h"
#include "app/Theme.h"
#include <imgui.h>
namespace simxr::app
{
void DrawLiveField(bool pending, std::string_view rejection)
{
	const auto minimum = ImGui::GetItemRectMin(), maximum = ImGui::GetItemRectMax();
	if (pending)
	{
		const float radius = ImGui::GetFontSize() * .2f;
		const ImVec2 center{maximum.x - radius * 2, minimum.y + radius * 2};
		ImGui::GetWindowDrawList()->PathArcTo(center, radius, static_cast<float>(ImGui::GetTime() * 6),
											  static_cast<float>(ImGui::GetTime() * 6 + 4));
		ImGui::GetWindowDrawList()->PathStroke(ImGui::GetColorU32(ImGuiCol_CheckMark), 0, 2);
	}
	if (!rejection.empty())
	{
		ImGui::GetWindowDrawList()->AddRect(minimum, maximum, theme::Record);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%.*s", static_cast<int>(rejection.size()), rejection.data());
	}
}
} // namespace simxr::app
