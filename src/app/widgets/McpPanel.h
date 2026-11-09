#pragma once

#include "app/mcp/McpUiState.h"
#include "app/services/CompanionSettings.h"

#include <string>

namespace simxr::app
{
std::string McpStatusText(const McpUiState& mcp);

// MCP service controls shared by the toolbar's MCP popover and Preferences.
class McpPanel
{
public:
	// Returns whether the settings changed.
	bool Draw(CompanionSettings& settings, McpUiState& mcp);

private:
	int m_client = 0;
	std::string m_clipboardError;
};
} // namespace simxr::app
