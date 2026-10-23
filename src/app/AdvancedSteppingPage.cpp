#include "app/AdvancedSteppingPage.h"
#include "Pch.h"
#include "app/pages/DeviceControlPanel.h"
#include "app/services/RuntimeSession.h"
#include "app/widgets/Controls.h"
#include "control/DebugProtocol.h"

#include <imgui.h>

namespace simxr::app
{
namespace wire = control;
namespace tag = wire::tag;
using panel::Boolean;
using panel::Nested;
using panel::Text;
using panel::U32;
using panel::U64;
void AdvancedSteppingPage::Draw()
{
	auto& debugger = m_session.Debugger();
	auto& workspace = m_session.Workspace();
	auto& preview = m_session.Preview();
	const auto selected = debugger.Selected();
	const auto& status = debugger.Status();
	ImGui::BeginDisabled(!m_session.Connected() || !m_session.Time().following || !m_session.SupportsEdits());
	if (auto edit = m_devices.DrawAdvanced())
		m_session.SubmitEdit(*edit);
	ImGui::EndDisabled();
	ImGui::BeginDisabled(!selected.id || !m_session.Connected());
	if (ImGui::Button("Cancel pending operation"))
		m_session.CancelPending();
	if (Section("Advanced stepping"))
	{
		if (ImGui::Button("Advance display only"))
			m_session.Command(wire::CommandKind::DisplayStep);
		ImGui::SetItemTooltip("Advances presentation without admitting another application frame");
		if (ImGui::Button("Strict frame admission"))
			m_session.Command(wire::CommandKind::StrictAdmission);
		SectionLabel("Capture a submitted source");
		ImGui::TextUnformatted("Submitted layer index");
		ImGui::SetNextItemWidth(-1);
		ImGui::InputInt("##Source layer", &m_rawLayer);
		ImGui::TextUnformatted("Projection view index");
		ImGui::SetNextItemWidth(-1);
		ImGui::InputInt("##Source view", &m_rawView);
		if (ImGui::Button("Advance and capture source"))
			m_session.Command(wire::CommandKind::Step, 0, true, static_cast<std::uint32_t>(std::max(0, m_rawLayer)),
							  static_cast<std::uint32_t>(std::max(0, m_rawView)));
	}
	ImGui::EndDisabled();
	SectionLabel("Runtime progress");
	if (!selected.id)
		ImGui::TextUnformatted("Session unavailable");
	else
		ImGui::Text("Admission: %s; controller: %s", debugger.Manual() ? "manual" : "automatic",
					debugger.HasLease() ? "this connection" : "observer");
	const auto deviations = wire::DecodeCompatibilityDeviations(status);
	ImGui::Text("Compatibility deviations: %s", !deviations ? "Unknown" : deviations->empty() ? "None" : "Active");
	ImGui::SetItemTooltip("Named API exceptions reported by the runtime; execution mode is separate");
	if (deviations)
		for (const auto& name : *deviations)
			ImGui::BulletText("%s", name.c_str());
	const auto pipeline = Nested(status.Find(tag::Pipeline));
	if (!selected.id && status.Find(tag::Pipeline))
		ImGui::TextUnformatted("Last observed pipeline:");
	if (status.Find(tag::Pipeline))
		ImGui::Text("Permits: %u; wait pending: %s", U32(pipeline, tag::Permits),
					Boolean(pipeline, tag::WaitPending) ? "yes" : "no");
	for (const auto field : {tag::Waited, tag::Begun})
		if (const auto value = pipeline.Find(field))
			ImGui::Text("%s frame: %llu", field == tag::Waited ? "Waited" : "Begun",
						static_cast<unsigned long long>(U64(Nested(value), tag::FrameId)));
	const auto presentation = Nested(status.Find(tag::Presentation));
	const auto display = Nested(presentation.Find(tag::Display));
	if (status.Find(tag::Presentation))
		ImGui::Text("Last submitted: %llu; display tick: %llu; selection: %s",
					static_cast<unsigned long long>(U64(presentation, tag::LastSubmitted)),
					static_cast<unsigned long long>(U64(display, tag::Tick)), Text(display, tag::Kind).c_str());
	for (const auto& operation : debugger.Operations())
		ImGui::Text("Operation %llu: %s / %s", static_cast<unsigned long long>(operation.id), operation.phase.c_str(),
					operation.progress.c_str());

	SectionLabel("Snapshots");
	bool autoSelect = m_session.AutoSelect();
	if (ImGui::Checkbox("Select completed steps", &autoSelect))
		m_session.SetAutoSelect(autoSelect);
	const auto selectedRecord = workspace.Selected() ? workspace.Selected()->local : 0;
	for (const auto& capture : workspace.Captures())
	{
		const auto label = "Capture " + std::to_string(capture.id) + " / frame " + std::to_string(capture.frame) +
						   " / " + capture.phase + (capture.released ? " (released)" : "");
		ImGui::PushID(static_cast<int>(capture.local));
		if (ImGui::Selectable(label.c_str(), capture.local == selectedRecord))
			m_session.SelectFrame(capture.local);
		ImGui::PopID();
	}
	SectionLabel("Private GPU images");
	for (const auto& image : preview.Images())
	{
		const auto label = "Frame " + std::to_string(image.frame) +
						   (image.live ? " / live" : " / capture " + std::to_string(image.capture));
		ImGui::PushID(static_cast<int>(image.id));
		if (ImGui::Selectable(label.c_str(), image.id == preview.SelectedId()))
			m_session.SelectImage(image.id);
		ImGui::PopID();
	}
	const auto capture = workspace.Selected();
	if (capture && !capture->released && capture->scope == debugger.Scope() &&
		m_session.Supports(wire::Capability::SnapshotPins))
	{
		const bool pinned = Boolean(capture->metadata, tag::Pinned);
		if (ImGui::Button(pinned ? "Unpin snapshot" : "Pin snapshot"))
			debugger.Command(pinned ? wire::CommandKind::UnpinSnapshot : wire::CommandKind::PinSnapshot, capture->id);
	}
	if (ImGui::Button("Release selected snapshot"))
		m_session.ReleaseSelected();
	ImGui::BeginDisabled(!capture || capture->released || capture->scope != debugger.Scope());
	if (ImGui::Button("Refresh frame details"))
		workspace.InspectSelected();
	ImGui::EndDisabled();
}
} // namespace simxr::app
