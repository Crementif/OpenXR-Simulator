#pragma once

#include "app/RuntimeView.h"

#include <imgui.h>
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

  private:
	void DrawImage(ShellState& shell, const RuntimeView& view, bool& pilotArea, float headerStart);
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
	// The header row's width last frame, so it can align with the image without clipping.
	float m_headerWidth = 0;
	int m_slice = 0;
	bool m_fit = true, m_panning = false;
	std::string m_pilotError;
};
} // namespace simxr::app
