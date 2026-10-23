#pragma once

#include "app/render/PreviewImages.h"
#include "app/RuntimeView.h"

#include <imgui.h>
#include <optional>
#include <string>

namespace simxr::app
{
class DeviceControlPanel;
class PilotController;
class RuntimeSession;
class SpatialEditor;
class Window;
// The preview region: state header, image with zoom and pan, device overlays
// and the pilot HUD.
class PreviewPanel
{
  public:
	PreviewPanel(RuntimeSession& session, PilotController& pilot, SpatialEditor& spatial, DeviceControlPanel& devices,
				 Window* window);
	// Returns whether the image may be piloted this frame.
	bool Draw(ShellState& shell, const RuntimeView& view);
	// The pinned region (x0, y0, x1, y1, normalized), shown instead of the whole
	// image across frames; the shell keeps one per launch profile.
	std::optional<ImVec4>& Crop()
	{
		return m_crop;
	}

  private:
	void DrawImage(ShellState& shell, const RuntimeView& view, bool& pilotArea);
	// Eye, zoom, crop and gizmo space, floating in the preview's top right corner.
	void DrawCorner(const PreviewImageSummary& image, bool live);
	// The single view button: eye, zoom and crop live in its popover.
	void DrawViewButton(const PreviewImageSummary& image);
	// Picking a crop: drag a rectangle over one eye's image.
	void PickCrop(ImVec2 imageMin, float eyeWidth, float height, int eyes);
	void DrawEmpty(ShellState& shell, const RuntimeView& view);
	// A downscaled image kept while running, for a past frame without an exact one.
	bool DrawKept(const RuntimeView& view);
	RuntimeSession& m_session;
	PilotController& m_pilot;
	SpatialEditor& m_spatial;
	DeviceControlPanel& m_devices;
	Window* m_window;
	float m_zoom = 1;
	ImVec2 m_previousImageSize{};
	// The corner controls' width and hover last frame, to place them and keep the wheel and pilot off them.
	float m_cornerWidth = 0;
	bool m_cornerHovered = false;
	int m_slice = 0;
	bool m_fit = true, m_panning = false, m_picking = false;
	std::optional<ImVec4> m_crop;
	std::optional<ImVec2> m_pickStart;
	std::string m_pilotError;
};
} // namespace simxr::app
