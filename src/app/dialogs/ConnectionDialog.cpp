#include "app/dialogs/ConnectionDialog.h"
#include "Pch.h"
#include "app/services/RuntimeSession.h"
#include "app/services/Utf8Path.h"
#include "app/widgets/Controls.h"
#include <imgui.h>

namespace simxr::app
{
ConnectionDialog::ConnectionDialog(std::string_view endpoint)
	: m_endpoint(std::max<std::size_t>(256, endpoint.size() + 1), 0)
{
	std::copy(endpoint.begin(), endpoint.end(), m_endpoint.begin());
}
static int ResizeEndpoint(ImGuiInputTextCallbackData* data)
{
	auto& endpoint = *static_cast<std::vector<char>*>(data->UserData);
	endpoint.resize(static_cast<std::size_t>(data->BufSize));
	data->Buf = endpoint.data();
	return 0;
}
void ConnectionDialog::Draw(RuntimeSession& session, bool* open)
{
	const auto selected = session.Debugger().Selected();
	ImGui::SetNextWindowSize({600, 400}, ImGuiCond_FirstUseEver);
	ImGui::OpenPopup("Runtime connection");
	if (!ImGui::BeginPopupModal("Runtime connection", open))
		return;
	const auto state = session.Connection();
	ImGui::TextUnformatted(session.View().connectionState.c_str());
	if (ImGui::Button("Refresh applications"))
		session.RefreshEndpoints();
	if (state.state == RuntimeConnectionState::Connected)
	{
		ImGui::SameLine();
		if (ImGui::Button("Disconnect"))
			session.Disconnect();
	}
	if (session.Endpoints().empty())
		ImGui::TextUnformatted("No applications");
	for (const auto& path : session.Endpoints())
	{
		if (ImGui::Selectable(PathText(Utf8Path(path).filename()).c_str(), path == session.Endpoint()))
			session.ConnectEndpoint(path);
		ImGui::SetItemTooltip("%s", path.c_str());
	}
	if (!state.detail.empty())
		ImGui::TextWrapped("%s", state.detail.c_str());
	for (const auto& entry : session.Debugger().Sessions())
	{
		const auto label = "Session " + std::to_string(entry.id) + "##" + std::to_string(entry.generation);
		if (ImGui::Selectable(label.c_str(), entry.id == selected.id && entry.generation == selected.generation))
			session.ChooseSession(entry);
		ImGui::SetItemTooltip("Run %llu", static_cast<unsigned long long>(entry.generation));
	}
	if (Section("Advanced connection"))
	{
		ImGui::SetNextItemWidth(-1);
		ImGui::InputText("##Endpoint", m_endpoint.data(), m_endpoint.size(), ImGuiInputTextFlags_CallbackResize,
						 ResizeEndpoint, &m_endpoint);
		if (ImGui::Button("Connect to endpoint"))
			session.ConnectEndpoint(m_endpoint.data());
		ImGui::TextWrapped("Queued: %zu. Replies: %zu. Exchange: %s", state.queued, state.ready,
						   state.inFlight ? "pending" : "idle");
	}
	ImGui::EndPopup();
}
} // namespace simxr::app
