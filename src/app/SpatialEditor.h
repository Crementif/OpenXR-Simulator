#pragma once
#include "app/ManualInputController.h"
#include "app/PlayerRig.h"
#include "app/ShellState.h"
#include "app/widgets/SpatialWidgets.h"

namespace simxr::app
{
class SpatialEditor
{
  public:
	SpatialEditor(PlayerRig& rig, ManualInputController& input);
	void Observe(const ManualInputSnapshot& snapshot, bool piloting);
	void SetContext(bool editable, bool lookingBack, std::span<const FrameView> eyes,
					const control::Message* historical, std::uint64_t historyIdentity);
	void Reset();
	// The rig view at the top of a device page.
	void DrawRigView(Selection& selection);
	// Rotation ball and reach map for the selected device, side by side when wide.
	void DrawPlacement(Selection& selection);
	void DrawRig(Selection& selection, ImVec2 size);
	bool DrawPreview(Selection& selection, ImVec2 minimum, ImVec2 maximum, const FrameView& camera, unsigned eye);
	void DrawExpanded(Selection& selection, ImVec2 size);
	ShellRect RotationBallRect() const
	{
		return m_rotationBallRect;
	}
	ShellRect ReachMapRect() const
	{
		return m_reachMapRect;
	}
	const std::array<ShellRect, 7>& MarkerRects() const
	{
		return m_markerRects;
	}
	bool Expanded() const
	{
		return m_view.expanded;
	}
	void SetExpanded(bool expanded)
	{
		m_view.expanded = expanded;
	}
	bool Active() const;
	std::uint64_t ObservedFrame() const
	{
		return m_observed ? m_observed->frame : 0;
	}
	bool Editable() const
	{
		return m_editable && m_observed.has_value();
	}
	void DrawSpaceToggle();

  private:
	SpatialScene LiveScene() const;
	std::array<bool, 7> EditableDevices(const SpatialScene& scene) const;
	void EditPose(int device, XrPosef pose, bool final);
	void ApplyResult(const SpatialEditResult& result, int device, XrPosef pose);
	PlayerRig& m_rig;
	ManualInputController& m_input;
	std::optional<ObservedSetup> m_observed;
	std::optional<CommandSource> m_source;
	std::optional<SpatialScene> m_historical;
	std::optional<FrameView> m_eye;
	std::uint64_t m_observation = 0, m_historyIdentity = 0;
	bool m_editable = false, m_lookingBack = false;
	std::string m_error;
	ShellRect m_rotationBallRect, m_reachMapRect;
	std::array<ShellRect, 7> m_markerRects{};
	RigViewState m_view;
	std::array<ArcballState, 7> m_arcballs;
	ReachMapState m_reach;
	std::array<GizmoState, 4> m_previewGizmos;
};
} // namespace simxr::app
