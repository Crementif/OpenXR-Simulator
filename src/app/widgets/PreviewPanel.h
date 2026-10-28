#pragma once

#include "app/render/PreviewImages.h"

#include <imgui.h>
#include <optional>
#include <string>

struct ImGuiWindow;

namespace simxr::app
{
struct RuntimeView;
struct ShellState;
class DeviceControlPanel;
class FramePage;
class PilotController;
class RuntimeSession;
class SpatialEditor;
class Window;
class PreviewPanel
{
  public:
	PreviewPanel(RuntimeSession& session, PilotController& pilot, SpatialEditor& spatial, FramePage& frame,
				 DeviceControlPanel& devices, Window* window);
	bool Draw(ShellState& shell, const RuntimeView& view, bool inputAvailable, bool automaticGamepad);
	void CancelInteraction();
	// The pinned region (x0, y0, x1, y1, normalized), shown instead of the whole
	// image across frames; the shell keeps one per launch profile.
	std::optional<ImVec4>& Crop()
	{
		return m_crop;
	}

  private:
	struct ImageArea
	{
		// Borrowed only until this Draw call finishes.
		ImGuiWindow* window;
		ImVec2 minimum, maximum;
		float eyeWidth;
		int eyes;
		bool hovered;
	};
	std::optional<ImageArea> DrawImage(ShellState& shell, const RuntimeView& view, bool inputAvailable);
	void UpdateImage(const ImageArea& area, const PreviewImageSummary& image, bool cornerHovered, bool pilotArea,
					 bool automaticGamepad);
	bool DrawCorner(const PreviewImageSummary& image, bool live, bool imageVisible);
	void DrawViewButton(const PreviewImageSummary& image, bool imageVisible, bool compact);
	void PickCrop(const ImageArea& area, bool hovered);
	bool SameGestureTarget(const PreviewImageSummary& image) const;
	void DrawEmpty(ShellState& shell, const RuntimeView& view);
	bool DrawKept(const RuntimeView& view);
	RuntimeSession& m_session;
	PilotController& m_pilot;
	SpatialEditor& m_spatial;
	FramePage& m_frame;
	DeviceControlPanel& m_devices;
	Window* m_window;
	float m_zoom = 1;
	ImVec2 m_previousImageSize{};
	int m_lastDraw = -1;
	int m_slice = 0;
	bool m_fit = true, m_panning = false, m_picking = false;
	std::optional<ImVec4> m_crop;
	std::optional<ImVec2> m_pickStart;
	std::optional<PreviewImageSummary> m_gestureImage, m_lastImage;
	ImVec4 m_gestureCrop{}, m_pickBounds{};
	int m_gestureSlice = 0;
	std::string m_pilotError;
};
} // namespace simxr::app
