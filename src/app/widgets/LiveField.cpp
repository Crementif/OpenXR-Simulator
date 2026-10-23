#include "app/widgets/LiveField.h"
#include "app/Theme.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <numbers>
namespace simxr::app
{
namespace
{
std::string Format(const char* format, float value)
{
	char text[32];
	std::snprintf(text, sizeof(text), format, value);
	return text;
}
} // namespace
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
bool FieldActions(const char* id, const FieldValue& value)
{
	bool changed = false;
	const bool hovered = ImGui::IsItemHovered();
	if (value.modified)
	{
		// A short accent bar inside the left edge: this value is not its default.
		const auto minimum = ImGui::GetItemRectMin();
		const float height = ImGui::GetFrameHeight(), inset = height * .25f;
		ImGui::GetWindowDrawList()->AddRectFilled({minimum.x + 1, minimum.y + inset},
												  {minimum.x + 3, minimum.y + height - inset}, theme::Accent, 1);
	}
	const auto reset = [&](const std::function<void()>& restore)
	{
		if (restore)
		{
			restore();
			changed = true;
		}
	};
	if (hovered && value.modified && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
		reset(value.reset);
	const auto& io = ImGui::GetIO();
	// macOS reports Shift+wheel as horizontal.
	const float wheel = io.MouseWheel != 0 ? io.MouseWheel : io.KeyShift ? io.MouseWheelH : 0;
	if (hovered && io.KeyCtrl && wheel != 0 && value.nudge && !ImGui::IsAnyItemActive())
	{
		value.nudge(wheel * (io.KeyShift ? .1f : 1));
		changed = true;
	}
	if (ImGui::BeginPopupContextItem(id))
	{
		if (ImGui::MenuItem("Reset", "Middle-click", false, value.modified && value.reset))
			reset(value.reset);
		if (value.group && value.resetGroup)
		{
			const std::string label = std::string("Reset ") + value.group;
			if (ImGui::MenuItem(label.c_str()))
				reset(value.resetGroup);
		}
		if (!value.text.empty() || !value.groupText.empty())
			ImGui::Separator();
		if (!value.text.empty() && ImGui::MenuItem("Copy"))
			ImGui::SetClipboardText(value.text.c_str());
		if (value.group && !value.groupText.empty())
		{
			const std::string label = std::string("Copy ") + value.group;
			if (ImGui::MenuItem(label.c_str()))
				ImGui::SetClipboardText(value.groupText.c_str());
		}
		ImGui::EndPopup();
	}
	return changed;
}
FieldValue NumberValue(float& value, float fallback, float step, float minimum, float maximum, const char* format)
{
	FieldValue result;
	result.reset = [&value, fallback]
	{
		value = fallback;
	};
	result.modified = std::abs(value - fallback) > step * .01f;
	result.nudge = [&value, step, minimum, maximum](float steps)
	{
		value = std::clamp(value + steps * step, minimum, maximum);
	};
	result.text = Format(format, value);
	return result;
}
FieldValue AngleValue(float& radians, float fallback, float minimumDegrees, float maximumDegrees)
{
	constexpr float degrees = 180 / std::numbers::pi_v<float>;
	FieldValue result;
	result.reset = [&radians, fallback]
	{
		radians = fallback;
	};
	result.modified = std::abs(radians - fallback) * degrees > .05f;
	result.nudge = [&radians, minimumDegrees, maximumDegrees](float steps)
	{
		radians = std::clamp(radians * degrees + steps, minimumDegrees, maximumDegrees) / degrees;
	};
	result.text = Format("%.1f", radians * degrees);
	return result;
}
FieldValue SwitchValue(bool& value, bool fallback)
{
	FieldValue result;
	result.reset = [&value, fallback]
	{
		value = fallback;
	};
	result.modified = value != fallback;
	result.text = value ? "true" : "false";
	return result;
}
} // namespace simxr::app
