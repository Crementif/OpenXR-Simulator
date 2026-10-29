#include "app/widgets/SpatialEditor.h"
#include "app/UiPch.h"
#include "app/services/ManualInputController.h"
#include "app/services/PlayerRig.h"
#include "app/widgets/Controls.h"
#include "app/widgets/Theme.h"
#include "math/Math.h"
#include "simulation/TrackingSystem.h"

namespace simxr::app
{
SpatialEditor::SpatialEditor(PlayerRig& rig, ManualInputController& input, UndoStack& undo)
	: m_rig(rig), m_input(input), m_undo(undo)
{}

void SpatialEditor::Observe(const ManualInputSnapshot& snapshot, bool piloting)
{
	m_source = snapshot.source;
	if (snapshot.observation == m_observation)
		return;
	const bool first = m_observation == 0;
	m_observation = snapshot.observation;
	try
	{
		auto observed = ReadObservedSetup(snapshot.state);
		const bool changed =
			!m_observed ||
			!std::equal(observed.values.tracking.trackers.begin(), observed.values.tracking.trackers.end(),
						m_observed->values.tracking.trackers.begin(), m_observed->values.tracking.trackers.end(),
						[](const auto& a, const auto& b) { return a.identity == b.identity; });
		if (changed)
			Cancel();
		m_observed = std::move(observed);
		if (first)
			m_rig.Reset(*m_observed);
		else if (changed || (!piloting && !Active() && !snapshot.posePending))
			m_rig.Observe(*m_observed);
		m_error = snapshot.error;
	}
	catch (const std::exception& error)
	{
		m_observed.reset();
		m_error = error.what();
	}
}

void SpatialEditor::SetContext(bool editable, bool lookingBack, std::span<const FrameView> eyes,
							   const control::Message* historical, std::uint64_t historyIdentity)
{
	if (!editable || lookingBack)
		Cancel();
	m_editable = editable;
	m_lookingBack = lookingBack;
	m_markerRects = {};
	m_rotationBallRect = {};
	m_eye = eyes.empty() ? std::nullopt : std::optional{eyes.front()};
	if (historyIdentity != m_historyIdentity)
	{
		m_historyIdentity = historyIdentity;
		m_historical.reset();
	}
	if (historical && !m_historical)
		try
		{
			if (const auto tracking = ReadInspectionTracking(*historical))
				m_historical = SpatialScene::FromTracking(*tracking);
		}
		catch (const std::exception& error)
		{
			m_error = error.what();
		}
	if (m_historical)
		m_historical->eye = m_eye;
}

void SpatialEditor::Reset()
{
	Cancel();
	m_observed.reset();
	m_historical.reset();
	m_eye.reset();
	m_observation = m_historyIdentity = 0;
	m_arcballs = {};
	m_previewGizmos = {};
	m_view.gizmo = {};
	m_editable = false;
	m_lookingBack = false;
	m_source.reset();
	m_error.clear();
	m_selected = -1;
}

void SpatialEditor::Cancel()
{
	CancelPoseGizmo(m_view.gizmo);
	for (auto& state : m_previewGizmos)
		CancelPoseGizmo(state);
	for (auto& state : m_arcballs)
		CancelRotationOrb(state);
	m_view.orbiting = m_view.panning = false;
	m_view.joint = -1;
	if (m_editing)
	{
		m_input.DiscardUnsentPose();
		m_undo.FinishGesture();
		m_editing = false;
	}
}

void SpatialEditor::EndFrame()
{
	const int frame = ImGui::GetFrameCount();
	if (m_view.gizmo.drawnFrame != frame)
		CancelPoseGizmo(m_view.gizmo);
	if (m_view.drawnFrame != frame)
		m_view.orbiting = m_view.panning = false;
	for (auto& state : m_previewGizmos)
		if (state.drawnFrame != frame)
			CancelPoseGizmo(state);
	for (auto& state : m_arcballs)
		if (state.drawnFrame != frame)
			CancelRotationOrb(state);
	if (m_editing && !Active())
		Cancel();
}

void SpatialEditor::DrawRigView(Selection& selection)
{
	if (!std::holds_alternative<DeviceSel>(selection) || (!m_observed && !m_historical) || m_view.expanded)
		return;
	// A short drawer keeps room for the controls below the rig.
	const float scale = ImGui::GetFontSize() / 16;
	const auto available = ImGui::GetContentRegionAvail();
	DrawRig(selection, {available.x / scale, std::clamp(available.y / scale * .3f, 120.0f, 220.0f)});
}

void SpatialEditor::DrawPlacement(Selection& selection)
{
	PrepareSelection(selection);
	if (!std::holds_alternative<DeviceSel>(selection) || !m_observed || m_lookingBack)
		return;
	const float scale = ImGui::GetFontSize() / 16;
	auto scene = LiveScene();
	const auto editable = EditableDevices(scene);
	const int selected = SelectedDevice(selection);
	if (selected >= 0 &&
		(selected < 3 || static_cast<std::size_t>(selected - 3) < m_observed->values.tracking.trackers.size()))
	{
		SectionLabel("Rotation");
		XrQuaternionf reference = m_rig.Root().orientation;
		if (selected == 1 || selected == 2)
			reference = m_rig.AnchorPose(m_rig.HandAnchor(selected - 1)).orientation;
		else if (selected > 2)
		{
			const auto joint = m_rig.TrackerAt(selected - 3).joint;
			reference = joint ? m_rig.JointPose(*joint).orientation : IdentityPose().orientation;
		}
		const auto camera = RigCamera(m_view, scene, 1);
		auto pose = scene.devices[selected];
		const auto rotationMinimum = ImGui::GetCursorScreenPos();
		const float column = ImGui::GetContentRegionAvail().x;
		m_rotationBallRect = {rotationMinimum.x + std::max(0.0f, (column - 160 * scale) / 2), rotationMinimum.y,
							  160 * scale, 160 * scale};
		const auto model = selected == 0 ? OrbModel::Headset : selected < 3 ? OrbModel::Controller : OrbModel::Puck;
		ImGui::PushID(selected);
		const auto result =
			RotationOrb("Rotation ball", m_arcballs[selected], pose.orientation, camera.pose.orientation, reference,
						editable[selected], model, DeviceColor(selected), column);
		ImGui::PopID();
		ApplyResult(result, selected, pose);
		if (!editable[selected])
			ImGui::TextDisabled("Read only");
	}
	if (!m_error.empty() && ImGui::IsWindowHovered())
		ImGui::SetTooltip("%s", m_error.c_str());
}

void SpatialEditor::DrawRig(Selection& selection, ImVec2 size)
{
	PrepareSelection(selection);
	if (m_lookingBack && !m_historical)
	{
		const float scale = ImGui::GetFontSize() / 16;
		const auto origin = ImGui::GetCursorScreenPos();
		const ImVec2 extent{size.x * scale, size.y * scale};
		const auto text = ImGui::CalcTextSize("Pose unavailable");
		ImGui::GetWindowDrawList()->AddText(
			{origin.x + std::max(0.f, (extent.x - text.x) / 2), origin.y + std::max(0.f, (extent.y - text.y) / 2)},
			ImGui::GetColorU32(ImGuiCol_TextDisabled), "Pose unavailable");
		ImGui::Dummy(extent);
		return;
	}
	const auto live = LiveScene();
	auto scene = m_lookingBack ? *m_historical : live;
	int selected = SelectedDevice(selection);
	const int previous = selected;
	const auto result =
		RigView("Rig view", m_view, scene, selected, EditableDevices(scene), size, m_lookingBack ? &live : nullptr);
	if (selected != previous)
		SelectDevice(selection, selected);
	if (selected >= 3 && result.joint >= 0 && Editable() && EditableDevices(live)[selected])
	{
		const auto slot = static_cast<std::size_t>(selected - 3);
		m_rig.AttachTracker(slot, static_cast<BodyJoint>(result.joint));
		m_rig.SetTrackerOffset(slot, IdentityPose());
		Submit(result.finished);
	}
	else if (selected >= 0)
		ApplyResult(result, selected, scene.devices[selected]);
}

void SpatialEditor::DrawExpanded(Selection& selection, ImVec2 size)
{
	const float scale = ImGui::GetFontSize() / 16;
	DrawRig(selection, {size.x / scale, size.y / scale});
}

void SpatialEditor::DrawSpaceToggle()
{
	if (IconButton("Gizmo space", m_view.gizmo.local ? Icon::Axes : Icon::Globe,
				   m_view.gizmo.local ? "Local axes. Space selects move, rotate or both. Ctrl snaps."
									  : "World axes. Space selects move, rotate or both. Ctrl snaps."))
		m_view.gizmo.local = !m_view.gizmo.local;
	for (auto& gizmo : m_previewGizmos)
		gizmo.local = m_view.gizmo.local;
}

bool SpatialEditor::Active() const
{
	return m_view.gizmo.usingGizmo ||
		   std::any_of(m_arcballs.begin(), m_arcballs.end(),
					   [](const auto& state) { return state.dragging || state.activeField; }) ||
		   std::any_of(m_previewGizmos.begin(), m_previewGizmos.end(),
					   [](const auto& state) { return state.usingGizmo; });
}

ShellRect SpatialEditor::RigViewRect() const
{
	return {m_view.minimum.x, m_view.minimum.y, m_view.size.x, m_view.size.y};
}

std::array<ShellRect, MaxTrackerDevices + 3> SpatialEditor::RigDevicePoints() const
{
	std::array<ShellRect, MaxTrackerDevices + 3> result{};
	for (std::size_t device = 0; device < result.size(); ++device)
		result[device] = {m_view.points[device].x, m_view.points[device].y, 0, 0};
	return result;
}

int SpatialEditor::SelectedDevice(const Selection& selection)
{
	if (const auto device = std::get_if<DeviceSel>(&selection))
	{
		switch (device->page)
		{
		case DevicePage::Head:
			return 0;
		case DevicePage::Left:
			return 1;
		case DevicePage::Right:
			return 2;
		case DevicePage::Trackers:
			return 3 + static_cast<int>(std::min(device->slot, static_cast<unsigned>(MaxTrackerDevices - 1)));
		default:
			break;
		}
	}
	return -1;
}

void SpatialEditor::SelectDevice(Selection& selection, int device)
{
	if (device >= 0)
		selection = DeviceSel{device == 0	? DevicePage::Head
							  : device == 1 ? DevicePage::Left
							  : device == 2 ? DevicePage::Right
											: DevicePage::Trackers,
							  device > 2 ? static_cast<unsigned>(device - 3) : 0};
}

ControlChannel SpatialEditor::DeviceChannel(int device)
{
	return device == 0	 ? ControlChannel::Head
		   : device == 1 ? ControlChannel::LeftPose
		   : device == 2 ? ControlChannel::RightPose
						 : ControlChannel::Trackers;
}

void SpatialEditor::PrepareSelection(const Selection& selection)
{
	const int selected = SelectedDevice(selection);
	if (selected != m_selected)
	{
		Cancel();
		m_selected = selected;
	}
}

SpatialScene SpatialEditor::LiveScene() const
{
	if (!m_observed || !m_rig.Ready())
		return {};
	auto tracking = m_observed->values.tracking;
	tracking.head.pose = m_rig.Head();
	tracking.body = m_rig.Body();
	tracking.body.rootPose = m_rig.Root();
	for (std::size_t hand = 0; hand < 2; ++hand)
		tracking.controllers[hand].pose.pose = m_rig.Hand(hand);
	for (std::size_t slot = 0; slot < tracking.trackers.size(); ++slot)
	{
		tracking.trackers[slot].jointOffset.reset();
		tracking.trackers[slot].poseOverride = m_rig.Tracker(slot);
	}
	auto scene = SpatialScene::FromTracking(
		TrackingSystem::Evaluate(RuntimeTime{Duration{1}}, tracking, BodyModel{m_observed->values.proportions}));
	scene.root = m_rig.Root();
	scene.eye = m_lookingBack ? std::nullopt : m_eye;
	for (std::size_t slot = 0; slot < tracking.trackers.size(); ++slot)
		scene.labels[slot + 3] = TrackerRoleLetter(m_rig.TrackerDeviceAt(slot).role);
	scene.attachable = AttachableJoints();
	return scene;
}

std::array<bool, MaxTrackerDevices + 3> SpatialEditor::EditableDevices(const SpatialScene& scene) const
{
	std::array<bool, MaxTrackerDevices + 3> result{};
	if (!Editable() || m_lookingBack)
		return result;
	for (int device = 0; device < static_cast<int>(MaxTrackerDevices + 3); ++device)
	{
		const auto channel = DeviceChannel(device);
		const auto owner = m_observed->owners[static_cast<std::size_t>(channel)];
		result[device] = scene.available[device] && (!owner || owner == m_source);
		for (std::size_t scenario = 0; scenario < m_observed->scenarios.size(); ++scenario)
			if (m_observed->scenarios[scenario] &&
				ScenarioInputChannel(static_cast<FailureScenario>(scenario)) == channel)
				result[device] = false;
	}
	return result;
}

void SpatialEditor::EditPose(int device, XrPosef pose, bool final)
{
	if (!Editable() || device < 0 || device >= static_cast<int>(MaxTrackerDevices + 3) ||
		!EditableDevices(LiveScene())[device])
		return;
	if (device == 0)
		m_rig.SetHeadWorld(pose);
	else if (device < 3)
		m_rig.SetHandWorld(device - 1, pose);
	else
		m_rig.SetTrackerWorld(device - 3, pose);
	Submit(final);
}

void SpatialEditor::Submit(bool final)
{
	auto edit = m_rig.Transaction(*m_observed, m_source, true);
	edit.continuous = !final;
	if (m_input.Submit(edit))
	{
		m_undo.Record(*m_observed, edit);
		m_editing = !final;
	}
	else
		m_error = "Could not change the pose. Try again.";
}

void SpatialEditor::ApplyResult(const SpatialEditResult& result, int device, XrPosef pose)
{
	if (result.changed || result.finished)
		EditPose(device, pose, result.finished);
}
} // namespace simxr::app
