#pragma once

#include <imgui.h>

// The companion's colours and metrics (UI design guide §10). Colours are
// display-encoded sRGB; the renderer handles UNORM targets, so never linearise them.
namespace simxr::app::theme
{
inline constexpr ImU32 Bg = IM_COL32(19, 22, 27, 255);
inline constexpr ImU32 Surface = IM_COL32(26, 30, 37, 255);
inline constexpr ImU32 SurfaceRaised = IM_COL32(35, 40, 51, 255);
inline constexpr ImU32 Line = IM_COL32(44, 50, 61, 255);
inline constexpr ImU32 Text = IM_COL32(230, 233, 239, 255);
inline constexpr ImU32 TextDim = IM_COL32(139, 147, 161, 255);
inline constexpr ImU32 Accent = IM_COL32(47, 143, 163, 255);
inline constexpr ImU32 Paused = IM_COL32(240, 164, 58, 255);
inline constexpr ImU32 Past = IM_COL32(76, 141, 255, 255);
inline constexpr ImU32 Record = IM_COL32(229, 72, 77, 255);
inline constexpr ImU32 Ok = IM_COL32(61, 190, 122, 255);
inline constexpr ImU32 Warn = IM_COL32(229, 181, 58, 255);
inline constexpr ImU32 Left = IM_COL32(108, 168, 255, 255);
inline constexpr ImU32 Right = IM_COL32(255, 138, 101, 255);
inline constexpr ImU32 Head = IM_COL32(201, 167, 255, 255);
// Dark ink for labels drawn on top of coloured markers.
inline constexpr ImU32 Ink = IM_COL32(14, 20, 27, 255);
// 3D axis colours for gizmos, the rig view and the rotation ball.
inline constexpr ImU32 AxisX = IM_COL32(229, 72, 77, 255);
inline constexpr ImU32 AxisY = IM_COL32(61, 190, 122, 255);
inline constexpr ImU32 AxisZ = IM_COL32(76, 141, 255, 255);

inline constexpr float ControlHeight = 28;
inline constexpr float Radius = 4;
inline constexpr float ChipRadius = 12;

// The same colour with its alpha multiplied by opacity (0..1).
constexpr ImU32 Fade(ImU32 color, float opacity)
{
	const auto alpha = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * opacity;
	return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha < 0	  ? 0
															: alpha > 255 ? 255
																		  : alpha)
										 << IM_COL32_A_SHIFT);
}
// Linear blend between two colours, t in 0..1.
ImU32 Mix(ImU32 from, ImU32 to, float t);
inline ImVec4 Vec(ImU32 color)
{
	return ImGui::ColorConvertU32ToFloat4(color);
}
// Applies the tokens to an ImGui style at base (unscaled) metrics.
void Apply(ImGuiStyle& style);
} // namespace simxr::app::theme
