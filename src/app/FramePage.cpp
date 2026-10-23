#include "app/FramePage.h"
#include "app/DocumentTree.h"
#include "app/PastInput.h"
#include "app/RuntimeSession.h"
#include "app/SimulationEdits.h"
#include "app/SpatialEditor.h"
#include "app/Theme.h"
#include "app/widgets/Controls.h"
#include "control/InspectionDocument.h"

#include <algorithm>
#include <imgui.h>
#include <string>
#include <vector>

namespace simxr::app
{
namespace
{
namespace wire = control;
namespace tag = wire::tag;
using panel::Boolean;
using panel::Text;
using panel::U64;
std::vector<std::byte> InspectionMember(std::span<const std::byte> bytes, std::string_view name)
{
	wire::Message node;
	node.fields = wire::DecodeFields(bytes).fields;
	const auto data = node.Find(3);
	if (!data)
		return {};
	for (const auto& field : wire::DecodeFields(data->value).fields)
	{
		wire::Message child;
		child.fields = wire::DecodeFields(field.value).fields;
		if (Text(child, 1) == name)
			return field.value;
	}
	return {};
}
wire::Field InspectionScalar(std::span<const std::byte> bytes, std::string_view member)
{
	wire::Message node;
	node.fields = wire::DecodeFields(InspectionMember(bytes, member)).fields;
	return node.Find(3) ? *node.Find(3) : wire::Field{};
}
std::vector<std::vector<std::byte>> InspectionElements(std::span<const std::byte> bytes)
{
	wire::Message node;
	node.fields = wire::DecodeFields(bytes).fields;
	std::vector<std::vector<std::byte>> elements;
	if (const auto data = node.Find(3))
		for (const auto& page : wire::DecodeFields(data->value).fields)
			for (const auto& item : wire::DecodeFields(page.value).fields)
				elements.push_back(item.value);
	return elements;
}
const char* OwnerName(const CommandSource& source)
{
	switch (source.kind)
	{
	case CommandSourceKind::Manual:
		return "You";
	case CommandSourceKind::Rig:
		return "Script";
	case CommandSourceKind::Sequence:
		return "Replay";
	default:
		return "MCP";
	}
}
} // namespace
FramePage::FramePage(RuntimeSession& session, SpatialEditor& spatial, DocumentTree& documents)
	: m_session(session), m_spatial(spatial), m_documents(documents)
{
}
void FramePage::Draw(ShellState& shell, const RuntimeView& view)
{
	// The page shows exactly the selected frame; an empty FrameSel means the time cursor.
	auto selection = std::get<FrameSel>(shell.selection);
	if (!selection.identity.frame)
		selection = {view.time.cursor, view.time.epoch};
	const auto& identity = selection.identity;
	auto& workspace = m_session.Workspace();
	auto& preview = m_session.Preview();
	const auto matches = [&](const PanelScope& scope, std::uint64_t frame)
	{
		return scope.epoch == selection.epoch && scope.session == identity.session &&
			   scope.generation == identity.run && frame == identity.frame;
	};
	const auto selectedCapture = workspace.Selected();
	const auto capture =
		selectedCapture && matches(selectedCapture->scope, selectedCapture->frame) ? selectedCapture : nullptr;
	const auto field = capture ? capture->document.Find(tag::Document) : nullptr;
	const auto bytes = field ? std::span<const std::byte>{field->value} : std::span<const std::byte>{};
	const auto documentEpoch = capture ? capture->scope.epoch : 0;
	m_documents.Begin(documentEpoch);
	const std::string title = "Frame " + std::to_string(identity.frame);
	PageTitle(Icon::Live, title.c_str());
	if (selection.epoch != view.epoch || identity.session != view.session || identity.run != view.generation)
	{
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("earlier run");
		ImGui::SetItemTooltip("This frame belongs to a run that has ended; its images may be gone");
	}
	if (PageClose())
		shell.selection = std::monostate{};
	// The header stays visible while the frame contents scroll.
	ImGui::BeginChild("Frame details", {0, 0});
	if (!bytes.empty())
		m_spatial.DrawRig(shell.selection, {ImGui::GetContentRegionAvail().x / (ImGui::GetFontSize() / 16), 150});
	SectionLabel("Images");
	bool hasImage = false;
	for (const auto& image : preview.Images())
		if (matches(image.scope, image.frame) && image.ready && !image.released)
		{
			hasImage = true;
			ImGui::PushID(static_cast<int>(image.id));
			ImGui::Text("%s / %u x %u", image.live ? "Preview" : "Snapshot", image.description.width,
						image.description.height);
			preview.DrawImage(image.id, std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 8), 0);
			if (ImGui::IsItemClicked())
				m_session.SelectImage(image.id);
			ImGui::SetItemTooltip("Show in the preview");
			ImGui::PopID();
		}
	if (!hasImage)
		ImGui::TextDisabled("No image kept for this frame");
	if (capture && !capture->released && capture->scope == m_session.Debugger().Scope())
	{
		const bool pinned = Boolean(capture->metadata, tag::Pinned);
		if (ImGui::SmallButton(pinned ? "Unpin snapshot" : "Pin snapshot"))
			m_session.Debugger().Command(pinned ? wire::CommandKind::UnpinSnapshot : wire::CommandKind::PinSnapshot,
										 capture->id);
		ImGui::SetItemTooltip(pinned ? "Lets history drop this snapshot when it needs room"
									 : "Keeps this snapshot when history needs room");
	}
	if (const auto image = preview.Selected(); image && matches(image->scope, image->frame) && image->ready)
		if (Section("Export image"))
		{
			const auto& description = image->description;
			ImGui::TextDisabled("%u x %u, %u views", description.width, description.height, description.arrayLayers);
			ImGui::SetNextItemWidth(-1);
			ImGui::InputTextWithHint("##Image file", "File (.ppm or .rgba)", m_exportFile.data(), m_exportFile.size());
			ImGui::BeginDisabled(!image->reader || image->scope.epoch != m_session.Epoch() ||
								 m_session.Exports().Busy());
			if (ImGui::Button("Export"))
				m_session.RequestExport(m_exportFile.data());
			ImGui::EndDisabled();
			if (!m_session.Exports().Detail().empty())
				ImGui::TextDisabled("%s", m_session.Exports().Detail().c_str());
		}
	SectionLabel("Input at this frame");
	if (const auto index = m_session.History().Find(identity, selection.epoch))
		if (const auto* sample = m_session.InputSamples().Find(m_session.History().At(*index).input))
			DrawInputSummary(*sample);
	if (!bytes.empty())
	{
		if (const auto tracking = ReadInspectionTracking(capture->document))
		{
			const auto pose = [](const char* name, const PoseSample& value)
			{
				const auto& p = value.pose.position;
				ImGui::Text("%s  %.2f, %.2f, %.2f m", name, p.x, p.y, p.z);
			};
			pose("Head", tracking->head);
			pose("Left", tracking->controllers[0].pose);
			pose("Right", tracking->controllers[1].pose);
		}
		const auto input = InspectionMember(bytes, "input");
		const auto controllers = InspectionElements(InspectionMember(input, "controllers"));
		for (std::size_t hand = 0; hand < std::min<std::size_t>(2, controllers.size()); ++hand)
		{
			const auto& controls = controllers[hand];
			ImGui::Text("%s  Trigger %.2f / Grip %.2f", hand ? "Right" : "Left",
						InspectionScalar(controls, "trigger").AsF32().value_or(0),
						InspectionScalar(controls, "grip").AsF32().value_or(0));
			ImGui::Text("A %s / B %s", InspectionScalar(controls, "primary").AsBool().value_or(false) ? "down" : "up",
						InspectionScalar(controls, "secondary").AsBool().value_or(false) ? "down" : "up");
		}
		if (Section("All input values"))
			m_documents.Draw(input);
		const auto commands = InspectionMember(bytes, "applied_commands");
		wire::Message commandNode;
		commandNode.fields = wire::DecodeFields(commands).fields;
		const auto revisionId = U64(commandNode, 3);
		if (revisionId && documentEpoch == m_session.Epoch())
		{
			if (const auto revision = workspace.Revision(revisionId))
			{
				const auto setup = ReadObservedSetup(*revision);
				for (const auto& [label, channel] : {std::pair{"Head", ControlChannel::Head},
													{"Left", ControlChannel::LeftControls},
													{"Right", ControlChannel::RightControls}})
					if (const auto& owner = setup.owners[static_cast<std::size_t>(channel)])
						ImGui::Text("%s driven by %s", label, OwnerName(*owner));
			}
			else
				workspace.RequestRevision(revisionId, documentEpoch);
		}
		if (Section("Input owners and commands"))
			m_documents.Draw(commands);
	}
	else
		ImGui::TextDisabled(capture ? "Loading frame details" : "Head and hand details not kept for this frame");
	SectionLabel("Actions");
	if (!bytes.empty())
	{
		const auto actions = InspectionMember(bytes, "actions");
		const auto entries = InspectionElements(InspectionMember(actions, "actions"));
		const auto dictionaryId = InspectionScalar(actions, "dictionary").AsU64().value_or(0);
		std::vector<std::vector<std::byte>> definitions;
		if (documentEpoch == m_session.Epoch())
		{
			if (const auto dictionary = workspace.Revision(dictionaryId))
			{
				if (const auto document = dictionary->Find(tag::Document))
					definitions = InspectionElements(InspectionMember(document->value, "actions"));
			}
			else if (dictionaryId)
				workspace.RequestRevision(dictionaryId, documentEpoch);
		}
		ImGui::Text("%zu actions", entries.size());
		for (std::size_t index = 0; index < std::min<std::size_t>(8, entries.size()); ++index)
		{
			const auto& action = entries[index];
			const auto id = InspectionScalar(action, "id").AsU64().value_or(0);
			std::string name = "Action " + std::to_string(id);
			for (const auto& definition : definitions)
				if (InspectionScalar(definition, "id").AsU64() == id)
				{
					const auto value = InspectionScalar(definition, "name");
					if (value.AsText())
						name = std::string(*value.AsText());
					break;
				}
			ImGui::PushID(static_cast<int>(index));
			if (ImGui::TreeNode(name.c_str()))
			{
				for (const auto& value : InspectionElements(InspectionMember(action, "values")))
				{
					ImGui::Text("%s / bool %s / %.3f",
								InspectionScalar(value, "active").AsBool().value_or(false) ? "Active" : "Inactive",
								InspectionScalar(value, "boolean").AsBool().value_or(false) ? "true" : "false",
								InspectionScalar(value, "scalar").AsF32().value_or(0));
					m_documents.Draw(value);
				}
				m_documents.Draw(InspectionMember(action, "bindings"));
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if (Section("All actions and bindings"))
			m_documents.Draw(actions);
	}
	else
		ImGui::TextDisabled("Actions not kept for this frame");
	if (Section("Layers") && !bytes.empty())
		m_documents.Draw(InspectionMember(bytes, "layers"));
	if (Section("Timing"))
	{
		const auto& history = m_session.History();
		if (const auto index = history.Find(identity, selection.epoch))
		{
			const auto& frame = history.At(*index);
			const bool missed = frame.flags & (HistoryLate | HistoryDropped);
			ImGui::TextColored(theme::Vec(missed ? theme::Record : theme::Text), "Interval %.1f ms of %.1f ms budget%s",
							   frame.frameMilliseconds, frame.periodMilliseconds, missed ? " \u00b7 missed" : "");
			ImGui::SetItemTooltip("Time between this frame's display time and the previous one; the budget is the "
								  "display period");
			if (frame.cpuMilliseconds >= 0)
			{
				ImGui::Text("CPU %.1f ms / %+.1f ms spare", frame.cpuMilliseconds,
					frame.periodMilliseconds - frame.cpuMilliseconds);
				ImGui::SetItemTooltip("Host span from xrWaitFrame return to accepted xrEndFrame, including runtime submission");
			}
			else
				ImGui::TextDisabled("CPU timing unavailable");
			if (frame.compositorGpuMilliseconds >= 0)
			{
				ImGui::Text("Compositor GPU %.2f ms", frame.compositorGpuMilliseconds);
				ImGui::SetItemTooltip("GPU timestamps around composition of submitted layers; excludes app rendering");
			}
			else
				ImGui::TextDisabled("Compositor GPU timing unavailable");
			ImGui::Text("Run time %.3f s", frame.runSeconds);
		}
		if (!bytes.empty())
		{
			m_documents.Draw(InspectionMember(bytes, "prediction"));
			m_documents.Draw(InspectionMember(bytes, "frame_trace"));
		}
	}
	if (Section("Raw") && !bytes.empty())
	{
		if (ImGui::Button("Open in window"))
			m_documents.OpenWindow(bytes, documentEpoch);
		m_documents.Draw(bytes);
	}
	ImGui::EndChild();
}
} // namespace simxr::app
