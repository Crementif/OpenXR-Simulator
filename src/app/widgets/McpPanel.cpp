#include "app/UiPch.h"
#include "app/widgets/McpPanel.h"

namespace simxr::app
{
std::string McpStatusText(const McpUiState& mcp)
{
	if (!mcp.available)
		return "Unavailable in this session";
	if (mcp.stopping)
		return "Stopping";
	if (!mcp.listening)
		return mcp.error.empty() ? "Off" : "Unavailable";
	std::string text = mcp.sessions ? std::to_string(mcp.sessions) + (mcp.sessions == 1 ? " client" : " clients")
									: std::string("Waiting for a client");
	if (mcp.activeCalls)
		text += ", " + std::to_string(mcp.activeCalls) + (mcp.activeCalls == 1 ? " call running" : " calls running");
	return text;
}

bool McpPanel::Draw(CompanionSettings& settings, McpUiState& mcp)
{
	if (!mcp.available)
	{
		ImGui::TextWrapped("MCP controls are unavailable in this session.");
		return false;
	}
	bool dirty = false;
	ImGui::Text("Status: %s", McpStatusText(mcp).c_str());
	if (mcp.listening && !mcp.stopping)
		ImGui::SetItemTooltip("Sessions may remain listed until they are closed or expire.");
	if (ImGui::Checkbox("Enable MCP on startup", &settings.mcpEnabled))
		dirty = true;
	ImGui::SetItemTooltip("Start the local MCP service with the simulator. Changes take effect now.");
	if (mcp.listening && !mcp.stopping)
	{
		if (mcp.startedForSession)
			ImGui::TextWrapped("Enabled for this session.");
		if (!mcp.discoverable)
			ImGui::TextWrapped("This development endpoint is not discoverable by the connector.");
		if (ImGui::Button("Stop MCP"))
		{
			dirty |= settings.mcpEnabled;
			settings.mcpEnabled = false;
			mcp.stopRequested = true;
		}
	}
	else if (!mcp.stopping)
	{
		if (!mcp.error.empty())
			ImGui::TextWrapped("%s", mcp.error.c_str());
		if (ImGui::Button(mcp.error.empty() ? "Start MCP" : "Retry MCP"))
		{
			dirty |= !settings.mcpEnabled;
			settings.mcpEnabled = true;
			mcp.retryRequested = true;
		}
	}
	ImGui::SetNextItemWidth(std::max(1.f, std::min(240 * settings.scale, ImGui::GetContentRegionAvail().x -
																			 ImGui::CalcTextSize("Client").x -
																			 ImGui::GetStyle().ItemInnerSpacing.x)));
	ImGui::Combo("Client", &m_client, "Generic JSON\0Codex\0");
	const auto& configuration = m_client == 1 ? mcp.codexConfiguration : mcp.configuration;
	ImGui::BeginDisabled(configuration.empty());
	if (ImGui::Button("Copy MCP configuration"))
		m_clipboardError = SDL_SetClipboardText(configuration.c_str()) ? "" : SDL_GetError();
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Copy a configuration targeting this app executable and its connector. The agent's first "
						  "tool call starts the simulator when it is not running.");
	if (!mcp.configurationError.empty())
		ImGui::TextWrapped("%s", mcp.configurationError.c_str());
	if (!m_clipboardError.empty())
		ImGui::TextWrapped("Clipboard: %s", m_clipboardError.c_str());
	if (!mcp.httpConfiguration.empty() && ImGui::TreeNode("Advanced HTTP"))
	{
		ImGui::TextWrapped("Direct HTTP is local to this computer.");
		if (ImGui::Button("Copy HTTP connection"))
			m_clipboardError = SDL_SetClipboardText(mcp.httpConfiguration.c_str()) ? "" : SDL_GetError();
		ImGui::SetItemTooltip("Includes a private credential. Share it only with trusted clients.");
		ImGui::TreePop();
	}
	return dirty;
}
} // namespace simxr::app
