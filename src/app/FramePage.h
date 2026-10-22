#pragma once

#include "app/RuntimeView.h"

#include <array>

namespace simxr::app
{
class DocumentTree;
class RuntimeSession;
class SpatialEditor;
// Inspector page for one selected frame: images, inputs, actions, layers and
// timing as the runtime recorded them.
class FramePage
{
  public:
	FramePage(RuntimeSession& session, SpatialEditor& spatial, DocumentTree& documents);
	void Draw(ShellState& shell, const RuntimeView& view);

  private:
	RuntimeSession& m_session;
	SpatialEditor& m_spatial;
	DocumentTree& m_documents;
	std::array<char, 512> m_exportFile{"snapshot.ppm"};
};
} // namespace simxr::app
