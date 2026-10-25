#include "Math.h"
#include "Pch.h"
#include "app/pages/DeviceControlPanel.h"
#include "app/services/PlayerRig.h"
#include "app/services/SpatialMath.h"
#include "app/widgets/Controls.h"
#include <cmath>
#include <cstdio>
#include <imgui_internal.h>
#include <iomanip>
#include <locale>
#include <numbers>
#include <sstream>

namespace simxr::app
{
void DeviceControlPanel::DrawHead()
{
	ImGui::Spacing();
	DrawOwner(ControlChannel::Session);
	ImGui::BeginDisabled(!CanControl(ControlChannel::Session));
	if (ImGui::Button("Recenter view", {-1, 0}))
		ApplyOwned(ControlChannel::Session, SetScenario{FailureScenario::Recenter, true});
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Makes your current view the forward direction. Does not move devices.");
	ImGui::EndDisabled();
	DrawOwner(ControlChannel::Head);
	ImGui::BeginDisabled(!CanControl(ControlChannel::Head) || ScenarioActive(ControlChannel::Head));
	DrawExactPose(0);
	ImGui::EndDisabled();
}
void DeviceControlPanel::DrawExactPose(int device)
{
	const float scale = ImGui::GetFontSize() / 16;
	const auto flags = ImGui::GetContentRegionAvail().x >= 340 * scale ? ImGuiTreeNodeFlags_DefaultOpen : 0;
	if (!Section("Position & rotation", flags) || !m_playerRig.Ready())
		return;
	const auto channel = device == 0   ? ControlChannel::Head
						 : device == 1 ? ControlChannel::LeftPose
						 : device == 2 ? ControlChannel::RightPose
									   : ControlChannel::Trackers;
	ImGui::BeginDisabled(!CanControl(channel));
	// The numbers are relative to what the device follows (its anchor or joint), or to the world.
	XrPosef base = m_playerRig.Root();
	const char* follows = "Body";
	if (device == 1 || device == 2)
	{
		const auto anchor = m_playerRig.HandAnchor(static_cast<std::size_t>(device - 1));
		base = m_playerRig.AnchorPose(anchor);
		follows = anchor == Anchor::Head ? "Head" : anchor == Anchor::World ? "World" : "Body";
	}
	else if (device > 2)
	{
		const auto joint = m_playerRig.TrackerAt(static_cast<std::size_t>(device - 3)).joint;
		base = joint ? m_playerRig.JointPose(*joint) : IdentityPose();
		follows = joint ? BodyJointName(*joint) : "World";
	}
	if (!m_exactAnchor)
		base = IdentityPose();
	const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	FieldLabel("Relative to");
	const std::string relative = std::string(m_exactAnchor ? follows : "World") + "##Relative";
	if (ImGui::SmallButton(relative.c_str()))
		m_exactAnchor = !m_exactAnchor;
	ImGui::SetItemTooltip("%s", m_exactAnchor ? "Click for world coordinates"
											  : "Click for coordinates relative to what the device follows");
	// Rotation as angles to edit, or as the raw quaternion to read and copy.
	static constexpr const char* forms[] = {"Angles", "Quaternion"};
	static constexpr const char* formTips[] = {"Yaw, pitch and roll in degrees",
											   "Read-only rotation as x, y, z, w."};
	bool& quaternion = m_quaternionView[static_cast<std::size_t>(device)];
	// Segments share the width equally, so size both for the longer label.
	const float formWidth = (ImGui::CalcTextSize("Quaternion").x + ImGui::GetStyle().FramePadding.x * 2) * 2;
	// Right-aligned beside the frame switch, or on its own row in a narrow inspector.
	ImGui::SameLine();
	if (ImGui::GetCursorPosX() + formWidth > right)
		ImGui::NewLine();
	ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - formWidth));
	ImGui::PushID("Rotation form");
	if (const int form = Segmented(forms, quaternion ? 1 : 0, formWidth, formTips); form >= 0)
		quaternion = form == 1;
	ImGui::PopID();
	const auto worldPose = [device](const PlayerRig& rig)
	{
		return device == 0	? rig.Head()
			   : device < 3 ? rig.Hand(static_cast<std::size_t>(device - 1))
							: rig.Tracker(static_cast<std::size_t>(device - 3));
	};
	auto pose = RelativePose(worldPose(m_playerRig), base);
	// Where Reset puts this device, in the same frame as the numbers, so each number resets alone.
	auto defaults = m_playerRig;
	if (device == 0)
		defaults.ResetHead();
	else if (device < 3)
		defaults.ResetHand(static_cast<std::size_t>(device - 1));
	else
		defaults.SnapTracker(static_cast<std::size_t>(device - 3));
	const auto fallback = RelativePose(worldPose(defaults), base);
	constexpr float degrees = 180 / std::numbers::pi_v<float>;
	const auto toDegrees = [](XrQuaternionf orientation)
	{
		const auto radians = YawPitchRoll(orientation);
		return std::array<float, 3>{radians.x * degrees, radians.y * degrees, radians.z * degrees};
	};
	auto angles = toDegrees(pose.orientation);
	const auto fallbackAngles = toDegrees(fallback.orientation);
	const float width = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
	// Resets only this device's pose; controls and connection stay.
	const auto reset = [this, device]
	{
		if (device == 0)
			m_playerRig.ResetHead();
		else if (device < 3)
			m_playerRig.ResetHand(static_cast<std::size_t>(device - 1));
		else
			m_playerRig.SnapTracker(static_cast<std::size_t>(device - 3));
		ApplyRig();
	};
	const auto& q = pose.orientation;
	std::ostringstream poseText;
	poseText.imbue(std::locale::classic());
	poseText << std::fixed << std::setprecision(4) << "{\"position\": [" << pose.position.x << ", " << pose.position.y
			 << ", " << pose.position.z << "], \"orientation\": [" << std::setprecision(5) << q.x << ", " << q.y << ", "
			 << q.z << ", " << q.w << "]}";
	char quaternionText[80];
	std::snprintf(quaternionText, sizeof(quaternionText), "Quaternion: x %.4f, y %.4f, z %.4f, w %.4f", q.x, q.y, q.z,
				  q.w);
	const auto value = [&](float& number, float fallbackNumber, float step, float limit, const char* format)
	{
		auto result = NumberValue(number, fallbackNumber, step, -limit, limit, format);
		result.group = "pose";
		result.resetGroup = reset;
		result.groupText = poseText.str();
		return result;
	};
	static constexpr const char* positionIds[] = {"##X", "##Y", "##Z"};
	static constexpr const char* positionFormats[] = {"X %.2f m", "Y %.2f m", "Z %.2f m"};
	float* positions[] = {&pose.position.x, &pose.position.y, &pose.position.z};
	const float fallbackPositions[] = {fallback.position.x, fallback.position.y, fallback.position.z};
	bool edited = false;
	for (int axis = 0; axis < 3; ++axis)
	{
		if (axis)
			ImGui::SameLine();
		ImGui::SetNextItemWidth(width);
		edited |= Field(positionIds[axis],
						ImGui::DragFloat(positionIds[axis], positions[axis], .005f, 0, 0, positionFormats[axis]), true,
						value(*positions[axis], fallbackPositions[axis], .01f, 100, "%.4f"));
		ImGui::SetItemTooltip("X right, Y up, Z back.\nMiddle-click resets. Ctrl+wheel nudges.");
	}
	bool turned = false;
	if (quaternion)
	{
		float components[] = {q.x, q.y, q.z, q.w};
		static constexpr const char* componentIds[] = {"##Qx", "##Qy", "##Qz", "##Qw"};
		static constexpr const char* componentFormats[] = {"x %.4f", "y %.4f", "z %.4f", "w %.4f"};
		const float quarter = (ImGui::GetContentRegionAvail().x - 3 * ImGui::GetStyle().ItemSpacing.x) / 4;
		for (int component = 0; component < 4; ++component)
		{
			if (component)
				ImGui::SameLine();
			ImGui::SetNextItemWidth(quarter);
			ImGui::InputScalar(componentIds[component], ImGuiDataType_Float, &components[component], nullptr, nullptr,
							   componentFormats[component], ImGuiInputTextFlags_ReadOnly);
			FieldValue copy;
			char text[24];
			std::snprintf(text, sizeof(text), "%.6f", components[component]);
			copy.text = text;
			copy.group = "pose";
			copy.resetGroup = reset;
			copy.groupText = poseText.str();
			FieldActions((m_fieldScope + componentIds[component]).c_str(), copy);
			ImGui::SetItemTooltip("Read only. Use Angles to change the rotation.");
		}
	}
	else
	{
		static constexpr const char* angleIds[] = {"##Yaw", "##Pitch", "##Roll"};
		static constexpr const char* angleFormats[] = {"Yaw %.0f\u00b0", "Pitch %.0f\u00b0", "Roll %.0f\u00b0"};
		for (int axis = 0; axis < 3; ++axis)
		{
			if (axis)
				ImGui::SameLine();
			ImGui::SetNextItemWidth(width);
			const float limit = axis == 1 ? 89.9f : 180.0f;
			auto& angle = angles[static_cast<std::size_t>(axis)];
			turned |= Field(angleIds[axis],
							ImGui::DragFloat(angleIds[axis], &angle, .5f, -limit, limit, angleFormats[axis],
											 ImGuiSliderFlags_AlwaysClamp),
							true, value(angle, fallbackAngles[static_cast<std::size_t>(axis)], 1, limit, "%.1f"));
			ImGui::SetItemTooltip("%s\nMiddle-click resets. Ctrl+wheel nudges.", quaternionText);
		}
	}
	if (turned)
		pose.orientation = EulerAngles(angles[0] / degrees, angles[1] / degrees, angles[2] / degrees);
	if ((edited || turned) && !m_edit)
	{
		pose = Compose(base, pose);
		if (device == 0)
			m_playerRig.SetHeadWorld(pose);
		else if (device < 3)
			m_playerRig.SetHandWorld(static_cast<std::size_t>(device - 1), pose);
		else
			m_playerRig.SetTrackerWorld(static_cast<std::size_t>(device - 3), pose);
		Apply(PlayerEdit(true).commands);
	}
	ImGui::EndDisabled();
}
ManualTransaction DeviceControlPanel::PlayerEdit(bool includeWorld) const
{
	return m_observed ? m_playerRig.Transaction(*m_observed, m_manualSource, includeWorld) : ManualTransaction{};
}
void DeviceControlPanel::ApplyRig(std::vector<SimulationCommand> extra, std::string label)
{
	auto commands = PlayerEdit(true).commands;
	for (auto& command : extra)
		commands.push_back(std::move(command));
	m_continuousEdit = false;
	Apply(std::move(commands), std::move(label));
}
void DeviceControlPanel::DrawBodyPoses()
{
	ImGui::BeginDisabled(!CanControl(ControlChannel::Body) || ScenarioActive(ControlChannel::Body) ||
						 !m_observed->bodyOverridesAvailable);
	static constexpr const char* tips[] = {"Lower the arms. Hands and attached trackers follow.",
										   "Extend the arms sideways. Hands and attached trackers follow.",
										   "Lower the arms to 45°. Hands and attached trackers follow.",
										   "Sit with hands on thighs. Attached trackers follow."};
	const auto current = m_playerRig.CurrentBodyPose();
	ImGui::PushID("Body pose");
	const int pose =
		Segmented(BodyPoseNames, current ? static_cast<int>(*current) : -1, ImGui::GetContentRegionAvail().x, tips);
	if (pose >= 0 && m_observed && m_playerRig.Ready())
	{
		m_playerRig.ApplyBodyPose(static_cast<BodyPose>(pose));
		ApplyRig({}, std::string("Body pose: ") + BodyPoseNames[pose]);
	}
	ImGui::PopID();
	ImGui::EndDisabled();
}
void DeviceControlPanel::DrawPlayer()
{
	DrawOwner(ControlChannel::Body);
	DrawBodyPoses();
	FieldLabel("Walk speed");
	ImGui::SliderFloat("##Walk speed", &m_playerRig.walkSpeed, .1f, 5.0f, "%.1f m/s");
	// Walk speed only paces piloting; it is not sent to the app.
	FieldActions("Walk speed", NumberValue(m_playerRig.walkSpeed, 1, .1f, .1f, 5, "%.1f"));
	ImGui::SetItemTooltip("Piloting speed.\nMiddle-click for 1 m/s. Ctrl+wheel nudges.");
	DrawOwner(ControlChannel::Head);
	ImGui::BeginDisabled(!CanControl(ControlChannel::Head) || ScenarioActive(ControlChannel::Head) ||
						 ((m_observed->bodyRootAvailable || m_observed->bodyOverridesAvailable) &&
						  (!CanControl(ControlChannel::Body) || ScenarioActive(ControlChannel::Body))));
	const float defaultHeight = m_observed ? m_observed->values.proportions.height : 1.7f;
	FieldLabel("Height");
	bool changed =
		Field("##Player height", ImGui::DragFloat("##Player height", &m_playerRig.height, .01f, .4f, 2.2f, "%.2f m"),
			  true, NumberValue(m_playerRig.height, defaultHeight, .01f, .4f, 2.2f));
	FieldLabel("Floor (m)");
	FieldValue floor;
	floor.reset = [&]
	{
		m_playerRig.position = {};
	};
	const auto& position = m_playerRig.position;
	floor.modified = position.x != 0 || position.y != 0 || position.z != 0;
	std::ostringstream floorText;
	floorText.imbue(std::locale::classic());
	floorText << std::fixed << std::setprecision(3) << position.x << ", " << position.y << ", " << position.z;
	floor.text = floorText.str();
	// One drag per axis, each named in its own box; the group is the one field.
	ImGui::BeginGroup();
	ImGui::PushID("Player position");
	const float gap = ImGui::GetStyle().ItemInnerSpacing.x, width = (ImGui::CalcItemWidth() - gap * 2) / 3;
	bool dragged = false;
	static constexpr const char* ids[] = {"##X", "##Y", "##Z"};
	static constexpr const char* formats[] = {"X  %.2f", "Y  %.2f", "Z  %.2f"};
	float* axes[] = {&m_playerRig.position.x, &m_playerRig.position.y, &m_playerRig.position.z};
	for (int axis = 0; axis < 3; ++axis)
	{
		if (axis)
			ImGui::SameLine(0, gap);
		ImGui::SetNextItemWidth(width);
		dragged |= ImGui::DragFloat(ids[axis], axes[axis], .01f, 0, 0, formats[axis]);
	}
	ImGui::PopID();
	ImGui::EndGroup();
	changed |= Field("##Player position", dragged, true, floor);
	if (changed && !m_edit)
		Apply(PlayerEdit().commands);
	ImGui::EndDisabled();
}
void DeviceControlPanel::DrawAnchor(std::size_t hand)
{
	int anchor = static_cast<int>(m_playerRig.HandAnchor(hand));
	static constexpr const char* names[] = {"Body", "Head", "World"};
	static constexpr const char* tips[] = {"Moves and turns with the player", "Follows the head",
										   "Stays put in the room"};
	ImGui::PushID("Anchor");
	FieldLabel("Follows");
	const int option = Segmented(names, anchor, ImGui::GetContentRegionAvail().x, tips);
	const bool changed = option >= 0 && option != anchor;
	if (changed)
		anchor = option;
	ImGui::PopID();
	if (changed)
		m_playerRig.SetHandAnchor(hand, static_cast<Anchor>(anchor));
}
void DeviceControlPanel::DrawHandPresets(std::size_t hand)
{
	static constexpr const char* names[] = {"Rest", "Forward", "Up"};
	static constexpr const char* tips[] = {"Hold this hand low in front.",
										   "Reach forward at eye level with this hand.",
										   "Point this hand straight up."};
	ImGui::PushID("Hand presets");
	const int preset = Segmented(names, -1, ImGui::GetContentRegionAvail().x, tips);
	if (preset >= 0 && m_observed && m_playerRig.Ready())
	{
		m_playerRig.Preset(hand, static_cast<HandPreset>(preset));
		ApplyRig({}, std::string(hand ? "Right hand: " : "Left hand: ") + names[preset]);
	}
	ImGui::PopID();
}
} // namespace simxr::app
