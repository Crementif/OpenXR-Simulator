#pragma once

#include "app/RuntimeView.h"
#include "app/widgets/SpatialWidgets.h"

#include <array>
#include <cstdint>
#include <optional>

namespace simxr::app
{
class DocumentTree;
class RuntimeSession;

class FramePage
{
public:
	FramePage(RuntimeSession& session, DocumentTree& documents);
	void Prepare(const Selection& selection, const RuntimeView& view);

	bool Active() const
	{
		return m_active;
	}

	bool Expanded() const
	{
		return m_rigView.expanded;
	}

	void SetExpanded(bool expanded)
	{
		m_rigView.expanded = expanded;
	}

	void DrawExpanded(ImVec2 size);
	ShellRect RigViewRect() const;
	std::array<ShellRect, MaxTrackerDevices + 3> RigDevicePoints() const;
	void Draw(ShellState& shell, const RuntimeView& view);

private:
	void DrawRig(ImVec2 size);
	RuntimeSession& m_session;
	DocumentTree& m_documents;
	std::optional<SpatialScene> m_scene;
	std::uint64_t m_capture = 0, m_inspection = 0;
	RigViewState m_rigView;
	int m_device = -1;
	bool m_active = false, m_invalidPose = false;
	std::array<char, 512> m_exportFile{"snapshot.png"};
};
} // namespace simxr::app
