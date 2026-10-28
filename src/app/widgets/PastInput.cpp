#include "app/widgets/PastInput.h"
#include "app/UiPch.h"
#include "app/widgets/Theme.h"
#include "app/services/PlayerRig.h"

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
static const char* Tracking(bool connected, bool tracked)
{
	return !connected ? "Disconnected" : tracked ? "Tracked" : "Not tracked";
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
	const auto joints = std::count_if(body.joints.begin(), body.joints.end(),
									  [](const auto& joint)
									  {
										  return joint.has_value();
									  });
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
