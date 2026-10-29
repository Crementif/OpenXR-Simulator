#pragma once

#include "app/services/FrameIdentity.h"

#include <cstdint>
#include <string>

namespace simxr::app
{
class RuntimePanels;
class UiRenderer;

// Finite qualification against the controlled runtime probe.
class RuntimeSmoke
{
public:
	explicit RuntimeSmoke(std::string exportPath);
	void Poll(RuntimePanels& panels, UiRenderer& renderer);
	bool Finished() const noexcept;

private:
	enum class Stage
	{
		Session,
		Paused,
		ComposedCapture,
		Inspection,
		RawCapture,
		RetainedCapture,
		Released,
		Teardown,
		RetainedSelection,
		Export,
		Complete
	};
	Stage m_stage = Stage::Session;
	std::uint64_t m_rawCapture = 0, m_rawLocal = 0, m_rawEpoch = 0, m_holdUntil = 0;
	FrameIdentity m_rawFrame;
	std::string m_exportPath;
};
} // namespace simxr::app
