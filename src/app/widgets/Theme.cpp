#include "app/widgets/Theme.h"
#include "Pch.h"

namespace simxr::app::theme
{
ImU32 Mix(ImU32 from, ImU32 to, float t)
{
	const auto a = Vec(from), b = Vec(to);
	return ImGui::ColorConvertFloat4ToU32(
		{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
}
void Apply(ImGuiStyle& style)
{
	ImGui::StyleColorsDark(&style);
	auto* colors = style.Colors;
	const auto accentDim = Mix(SurfaceRaised, Accent, .45f), accentSoft = Mix(SurfaceRaised, Accent, .25f);
	colors[ImGuiCol_Text] = Vec(Text);
	colors[ImGuiCol_TextDisabled] = Vec(TextDim);
	colors[ImGuiCol_WindowBg] = Vec(Bg);
	colors[ImGuiCol_ChildBg] = Vec(Fade(Bg, 0));
	colors[ImGuiCol_PopupBg] = Vec(Surface);
	colors[ImGuiCol_Border] = Vec(Line);
	colors[ImGuiCol_Separator] = Vec(Line);
	colors[ImGuiCol_FrameBg] = Vec(SurfaceRaised);
	colors[ImGuiCol_FrameBgHovered] = Vec(Mix(SurfaceRaised, Text, .08f));
	colors[ImGuiCol_FrameBgActive] = Vec(accentSoft);
	colors[ImGuiCol_Button] = Vec(SurfaceRaised);
	colors[ImGuiCol_ButtonHovered] = Vec(Mix(SurfaceRaised, Text, .1f));
	colors[ImGuiCol_ButtonActive] = Vec(accentDim);
	colors[ImGuiCol_Header] = Vec(accentSoft);
	colors[ImGuiCol_HeaderHovered] = Vec(Mix(SurfaceRaised, Text, .08f));
	colors[ImGuiCol_HeaderActive] = Vec(accentDim);
	colors[ImGuiCol_Tab] = Vec(Surface);
	colors[ImGuiCol_TabSelected] = Vec(accentSoft);
	colors[ImGuiCol_TabHovered] = Vec(accentDim);
	colors[ImGuiCol_TitleBg] = Vec(Surface);
	colors[ImGuiCol_TitleBgActive] = Vec(Surface);
	colors[ImGuiCol_SliderGrab] = Vec(Accent);
	colors[ImGuiCol_SliderGrabActive] = Vec(Mix(Accent, Text, .3f));
	colors[ImGuiCol_CheckMark] = Vec(Mix(Accent, Text, .35f));
	colors[ImGuiCol_ScrollbarBg] = Vec(Fade(Bg, 0));
	colors[ImGuiCol_ScrollbarGrab] = Vec(SurfaceRaised);
	colors[ImGuiCol_ModalWindowDimBg] = Vec(Fade(Bg, .6f));
	style.WindowPadding = {14, 12};
	style.FramePadding = {8, 5};
	style.FrameRounding = Radius;
	style.GrabRounding = Radius;
	style.PopupRounding = Radius;
	style.ChildRounding = Radius;
	style.ItemSpacing = {8, 7};
	style.WindowBorderSize = 0;
	style.ChildBorderSize = 0;
	style.PopupBorderSize = 1;
}
} // namespace simxr::app::theme
