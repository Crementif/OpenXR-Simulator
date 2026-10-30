#include "app/widgets/PastInput.h"
#include "app/UiPch.h"
#include "app/services/PlayerRig.h"
#include "app/services/SpatialMath.h"
#include "app/widgets/InputPad.h"
#include "app/widgets/Theme.h"

#include <iomanip>

namespace simxr::app
{
static constexpr float s_degrees = 180 / std::numbers::pi_v<float>;

static const char* PresetName(BodyPreset preset)
{
	switch (preset)
	{
	case BodyPreset::Standing:
		return "Standing";
	case BodyPreset::TPose:
		return "T pose";
	case BodyPreset::Seated:
		return "Seated";
	}
	return "Unknown";
}

static std::string Position(const XrVector3f& value)
{
	std::ostringstream text;
	text << std::fixed << std::setprecision(2) << value.x << ", " << value.y << ", " << value.z << " m";
	return text.str();
}

static std::string Rotation(const XrQuaternionf& value)
{
	const auto angles = YawPitchRoll(value);
	char text[64];
	std::snprintf(text, sizeof(text), "%.1f°, %.1f°, %.1f°", angles.x * s_degrees, angles.y * s_degrees,
				  angles.z * s_degrees);
	return text;
}

static const char* Tracking(bool connected, bool tracked)
{
	return !connected ? "Disconnected" : tracked ? "Tracked" : "Not tracked";
}

static bool PoseTracked(XrSpaceLocationFlags flags)
{
	return (flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) && (flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
}

// Dimmed key/value rows: the recorded values of a past frame are not editable.
class PastInputRows
{
public:
	explicit PastInputRows(const char* id) : m_open(ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp))
	{
		if (m_open)
			ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6);
	}

	PastInputRows(const PastInputRows&) = delete;
	PastInputRows& operator=(const PastInputRows&) = delete;

	~PastInputRows()
	{
		if (m_open)
			ImGui::EndTable();
	}

	void Row(const char* key, const std::string& value)
	{
		if (!m_open)
			return;
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextDisabled("%s", key);
		ImGui::TableNextColumn();
		ImGui::TextDisabled("%s", value.c_str());
	}

private:
	bool m_open;
};

static std::string Placement(const TrackerDevice& tracker)
{
	if (tracker.jointOffset)
		return "Joint offset " + Position(tracker.jointOffset->position);
	if (tracker.poseOverride)
		return Position(tracker.poseOverride->position);
	return tracker.anchor ? "On the body" : "Default";
}

static void Missing(const char* what)
{
	ImGui::TextDisabled("%s not recorded for this frame", what);
}

void DrawPastHead(const InputSample& sample)
{
	PastInputRows rows("Past head");
	rows.Row("State", Tracking(true, PoseTracked(sample.head.flags)));
	rows.Row("Position", Position(sample.head.pose.position));
	rows.Row("Rotation", Rotation(sample.head.pose.orientation));
}

void DrawPastController(const InputSample& sample, std::size_t hand)
{
	auto controls = sample.controls[hand];
	const auto& pose = sample.poses[hand];
	const auto color = hand ? theme::Right : theme::Left;
	const std::string_view profile = controls.physicalProfile;
	const bool touch = profile.find("oculus") != std::string_view::npos ||
					   profile.find("meta") != std::string_view::npos || profile.find("pico") != std::string_view::npos;
	const bool index = profile.find("valve/index") != std::string_view::npos;
	const char* primary = touch ? (hand ? "A##Primary" : "X##Primary") : index ? "A##Primary" : "1##Primary";
	const char* secondary = touch ? (hand ? "B##Secondary" : "Y##Secondary") : index ? "B##Secondary" : "2##Secondary";
	ImGui::PushID("Past controller");
	ImGui::PushID(static_cast<int>(hand));
	ImGui::BeginDisabled();
	bool latch = false;
	const auto top = ImGui::GetCursorPosY();
	StickPad("Stick", controls.stick.x, controls.stick.y, controls.thumbClick, latch, color);
	const float stickHeight = ImGui::GetItemRectSize().y;
	ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 2);
	const float unit = ImGui::GetFontSize() / 17, spacing = ImGui::GetStyle().ItemSpacing.y;
	ImGui::SetCursorPosY(top + std::max(0.0f, (stickHeight - (24 * 2 + 34) * unit - spacing * 2) / 2));
	ImGui::BeginGroup();
	const float width = std::max(60.f, ImGui::GetContentRegionAvail().x);
	TriggerBar("Trigger", "Trigger", controls.trigger, latch, color, width);
	TriggerBar("Grip", "Grip", controls.grip, latch, color, width);
	HoldButton("Primary", primary, controls.primary, latch, color);
	ImGui::SameLine();
	HoldButton("Secondary", secondary, controls.secondary, latch, color);
	ImGui::SameLine();
	HoldButton("Menu", "M##Menu", controls.menu, latch, color);
	ImGui::EndGroup();
	ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), top + stickHeight + spacing));
	ImGui::EndDisabled();
	ImGui::PopID();
	ImGui::PopID();
	PastInputRows rows("Past controller");
	rows.Row("State", Tracking(pose.connected && controls.connected, PoseTracked(pose.pose.flags)));
	rows.Row("Position", Position(pose.pose.pose.position));
	rows.Row("Rotation", Rotation(pose.pose.pose.orientation));
	std::string touched;
	const std::pair<std::string, bool> touches[] = {
		{"Trigger", controls.triggerTouch},		{"Stick", controls.thumbTouch},
		{{primary, 1}, controls.primaryTouch},	{{secondary, 1}, controls.secondaryTouch},
		{"Thumbrest", controls.thumbrestTouch}, {"Trackpad", controls.trackpadTouch}};
	for (const auto& [label, value] : touches)
		if (value)
			touched += (touched.empty() ? "" : ", ") + label;
	rows.Row("Touching", touched.empty() ? "Nothing" : touched);
	if (!profile.empty())
		rows.Row("Profile", std::string(profile.substr(profile.find_last_of('/') + 1)));
}

void DrawPastBody(const InputSample& sample)
{
	if (!sample.body)
	{
		Missing("Body");
		return;
	}
	const auto& body = *sample.body;
	PastInputRows rows("Past body");
	rows.Row("Active", body.active ? "Yes" : "No");
	rows.Row("Pose", PresetName(body.preset));
	char confidence[16];
	std::snprintf(confidence, sizeof(confidence), "%.2f", body.confidence);
	rows.Row("Confidence", confidence);
	if (sample.bodyRoot)
		rows.Row("Root", Position(sample.bodyRoot->position));
	static constexpr const char* s_controls[] = {"Hips", "Chest", "Head", "Left foot", "Right foot"};
	for (std::size_t index = 0; index < body.controls.size(); ++index)
	{
		const auto& control = body.controls[index];
		if (control.offset || !control.tracked)
			rows.Row(s_controls[index], (control.offset ? Position(*control.offset) + " " : std::string{}) +
											(control.tracked ? "" : "not tracked"));
	}
	const auto joints =
		std::count_if(body.joints.begin(), body.joints.end(), [](const auto& joint) { return joint.has_value(); });
	if (joints)
		rows.Row("Joints", std::to_string(joints) + " overridden");
}

void DrawPastGaze(const InputSample& sample)
{
	if (!sample.gaze)
	{
		Missing("Gaze");
		return;
	}
	const auto& gaze = *sample.gaze;
	PastInputRows rows("Past gaze");
	char angle[32];
	std::snprintf(angle, sizeof(angle), "%.1f°", gaze.yaw * s_degrees);
	rows.Row("Yaw", angle);
	std::snprintf(angle, sizeof(angle), "%.1f°", gaze.pitch * s_degrees);
	rows.Row("Pitch", angle);
	rows.Row("State", !gaze.available ? "Unavailable" : gaze.tracked ? "Tracked" : "Not tracked");
}

void DrawPastTrackers(const InputSample& sample, std::size_t selected)
{
	if (!sample.trackers)
	{
		Missing("Trackers");
		return;
	}
	if (!ImGui::BeginTable("Past trackers", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
		return;
	for (std::size_t slot = 0; slot < sample.trackers->size(); ++slot)
	{
		const auto& tracker = (*sample.trackers)[slot];
		ImGui::TableNextRow();
		if (slot == selected)
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, theme::Fade(theme::Past, .18f));
		ImGui::TableNextColumn();
		ImGui::TextDisabled("%s", TrackerRoleName(tracker.role));
		ImGui::TableNextColumn();
		ImGui::TextDisabled("%s", Tracking(tracker.connected, tracker.tracked));
		ImGui::TableNextColumn();
		ImGui::TextDisabled("%s", Placement(tracker).c_str());
	}
	ImGui::EndTable();
}

void DrawInputSummary(const InputSample& sample)
{
	if (sample.body)
		ImGui::Text("Body  %s%s", PresetName(sample.body->preset), sample.body->active ? "" : " (inactive)");
	if (sample.gaze)
		ImGui::Text("Gaze  %.1f°, %.1f°%s", sample.gaze->yaw * s_degrees, sample.gaze->pitch * s_degrees,
					!sample.gaze->available ? " (unavailable)"
					: !sample.gaze->tracked ? " (not tracked)"
											: "");
	if (sample.trackers)
		for (const auto& tracker : *sample.trackers)
			if (tracker.connected)
				ImGui::Text("%s  %s%s", TrackerRoleName(tracker.role), Placement(tracker).c_str(),
							tracker.tracked ? "" : " (not tracked)");
	if (!sample.body && !sample.gaze && !sample.trackers)
		ImGui::TextDisabled("Body, gaze and trackers not recorded");
}
} // namespace simxr::app
