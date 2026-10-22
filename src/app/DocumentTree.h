#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace simxr::app
{
class RuntimeSession;
// Draws runtime inspection documents as an expandable tree and owns the
// floating Raw metadata window. Revisions resolve only within the epoch the
// document came from.
class DocumentTree
{
  public:
	explicit DocumentTree(RuntimeSession& session) : m_session(session)
	{
	}
	// Starts a drawing pass for documents of one connection epoch.
	void Begin(std::uint64_t documentEpoch);
	void Draw(std::span<const std::byte> bytes);
	void OpenWindow(std::span<const std::byte> bytes, std::uint64_t documentEpoch);
	void DrawWindow();

  private:
	void Node(std::span<const std::byte> bytes, unsigned depth);
	RuntimeSession& m_session;
	std::uint32_t m_nodesDrawn = 0;
	std::uint64_t m_documentEpoch = 0, m_windowEpoch = 0;
	std::vector<std::byte> m_windowDocument;
	bool m_windowOpen = false;
};
} // namespace simxr::app
