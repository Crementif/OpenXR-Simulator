#pragma once

#include "app/widgets/Icons.h"

#include <imgui.h>
#include <span>

namespace simxr::app
{
bool IconButton(const char* id, Icon icon, const char* tooltip, bool toggled = false, const char* label = nullptr,
				ImU32 tint = 0);
int Segmented(std::span<const char* const> labels, int selected, float width,
			  std::span<const char* const> tooltips = {});
void PageTitle(Icon icon, const char* title);
bool PageClose();
void FieldLabel(const char* label);
void SectionLabel(const char* label);
bool Section(const char* label, ImGuiTreeNodeFlags flags = 0);
struct RailStatus
{
	ImU32 dot = 0;
	const char* dotTooltip = nullptr;
	const char* badge = nullptr;
	ImU32 badgeColor = 0;
	const char* key = nullptr;
	bool pilotKey = true;
	bool iconOnly = false;
	ImU32 tint = 0;
};
bool RailItem(const char* label, Icon icon, bool selected, const RailStatus& status, float height = 0);
bool DeviceTab(const char* label, Icon icon, bool selected, const RailStatus& status, ImVec2 size, bool named);
} // namespace simxr::app
