#include "app/widgets/DocumentTree.h"
#include "app/UiPch.h"
#include "app/services/RuntimeSession.h"
#include "control/InspectionDocument.h"

namespace simxr::app
{
namespace wire = control;
namespace tag = wire::tag;
static constexpr std::uint32_t s_maximumNodes = 2048;
void DocumentTree::Begin(std::uint64_t documentEpoch)
{
	m_nodesDrawn = 0;
	m_documentEpoch = documentEpoch;
}
void DocumentTree::Draw(const char* id, std::span<const std::byte> bytes)
{
	ImGui::PushID(id);
	Node(bytes, 0);
	ImGui::PopID();
}
void DocumentTree::OpenWindow(std::span<const std::byte> bytes, std::uint64_t documentEpoch)
{
	m_windowDocument.assign(bytes.begin(), bytes.end());
	m_windowEpoch = documentEpoch;
	m_windowOpen = true;
}
void DocumentTree::DrawWindow()
{
	if (!m_windowOpen)
		return;
	ImGui::SetNextWindowSize({600, 480}, ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Raw metadata", &m_windowOpen))
	{
		Begin(m_windowEpoch);
		Draw("Document", m_windowDocument);
	}
	ImGui::End();
}
void DocumentTree::Node(std::span<const std::byte> bytes, unsigned depth)
{
	if (bytes.empty())
		return;
	if (depth > 20 || ++m_nodesDrawn > s_maximumNodes)
	{
		if (m_nodesDrawn == s_maximumNodes + 1)
			ImGui::TextDisabled("Showing %u fields. Collapse some to see more.", s_maximumNodes);
		return;
	}
	auto decoded = wire::DecodeFields(bytes);
	wire::Message node;
	node.fields = std::move(decoded.fields);
	if (decoded.error != wire::DecodeError::None)
	{
		ImGui::TextUnformatted("Invalid inspection node");
		return;
	}
	const auto name = panel::Text(node, 1, "");
	const auto type = static_cast<wire::InspectionType>(panel::U32(node, 2));
	const auto data = node.Find(3);
	if (!data)
		return;
	const auto label = name.empty() ? "state" : name.c_str();
	if (type == wire::InspectionType::Object || type == wire::InspectionType::Array)
	{
		if (ImGui::TreeNode("##Node", "%s", label))
		{
			const auto entries = wire::DecodeFields(data->value);
			if (entries.error != wire::DecodeError::None)
				ImGui::TextDisabled("Invalid inspection fields");
			for (const auto& child : entries.fields)
			{
				ImGui::PushID(static_cast<int>(child.tag));
				if (type == wire::InspectionType::Object)
					Node(child.value, depth + 1);
				else
				{
					const auto page = wire::DecodeFields(child.value);
					if (page.error != wire::DecodeError::None)
						ImGui::TextDisabled("Invalid inspection page");
					for (const auto& item : page.fields)
					{
						ImGui::PushID(static_cast<int>(item.tag));
						Node(item.value, depth + 1);
						ImGui::PopID();
						if (m_nodesDrawn > s_maximumNodes)
							break;
					}
				}
				ImGui::PopID();
				if (m_nodesDrawn > s_maximumNodes)
					break;
			}
			ImGui::TreePop();
		}
	}
	else if (type == wire::InspectionType::Revision)
	{
		const auto id = data->AsU64().value_or(0);
		auto& workspace = m_session.Workspace();
		const bool current = m_documentEpoch == m_session.Epoch();
		ImGui::Text("%s: revision %llu", label, static_cast<unsigned long long>(id));
		ImGui::SameLine();
		ImGui::BeginDisabled(!current);
		if (ImGui::SmallButton("Inspect revision"))
			workspace.RequestRevision(id, m_documentEpoch);
		ImGui::EndDisabled();
		if (current)
			if (const auto revision = workspace.Revision(id, m_documentEpoch))
				if (auto document = revision->Find(tag::Document))
					Node(document->value, depth + 1);
	}
	else if (type == wire::InspectionType::Text)
		ImGui::TextWrapped("%s: %s", label, data->AsText() ? std::string(*data->AsText()).c_str() : "invalid text");
	else if (type == wire::InspectionType::Unsigned)
		ImGui::Text("%s: %llu", label, static_cast<unsigned long long>(data->AsU64().value_or(0)));
	else if (type == wire::InspectionType::Signed)
		ImGui::Text("%s: %lld", label, static_cast<long long>(data->AsI64().value_or(0)));
	else if (type == wire::InspectionType::Float)
		ImGui::Text("%s: %.7g", label, static_cast<double>(data->AsF32().value_or(0)));
	else if (type == wire::InspectionType::Boolean)
		ImGui::Text("%s: %s", label, data->AsBool().value_or(false) ? "true" : "false");
	else
		ImGui::Text("%s: unavailable", label);
}
} // namespace simxr::app
