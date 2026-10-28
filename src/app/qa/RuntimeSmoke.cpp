#include "app/qa/RuntimeSmoke.h"
#include "app/UiPch.h"
#include "app/RuntimePanels.h"
#include "app/render/UiRenderer.h"

namespace simxr::app
{
RuntimeSmoke::RuntimeSmoke(std::string exportPath) : m_exportPath(std::move(exportPath)) {}

bool RuntimeSmoke::Finished() const noexcept
{
	return m_stage == Stage::Complete;
}

void RuntimeSmoke::Poll(RuntimePanels& panels, UiRenderer& renderer)
{
	const auto state = panels.Snapshot();
	if (!state.error.empty() || state.phase == "failed")
		throw std::runtime_error("Runtime smoke operation failed: " +
								 (state.error.empty() ? state.progress : state.error));
	switch (m_stage)
	{
	case Stage::Session:
		if (state.sessionAvailable)
		{
			panels.Pause();
			m_stage = Stage::Paused;
		}
		break;
	case Stage::Paused:
		if (!state.operationPending && state.manual)
		{
			std::puts("READY companion production controls paused");
			std::fflush(stdout);
			panels.Command(control::CommandKind::Step);
			m_stage = Stage::ComposedCapture;
		}
		break;
	case Stage::ComposedCapture:
		if (!state.operationPending)
		{
			if (state.progress == "clear")
				panels.Command(control::CommandKind::Step);
			else if (state.selectedReady && state.selectedFrame == 3)
			{
				panels.InspectSelected();
				m_holdUntil = renderer.SubmittedFrames() + 4;
				m_stage = Stage::Inspection;
			}
		}
		break;
	case Stage::Inspection:
		if (state.inspectedFrame == 3 && renderer.CompletedFrames() >= m_holdUntil)
		{
			panels.Command(control::CommandKind::Step, 0, true);
			m_stage = Stage::RawCapture;
		}
		break;
	case Stage::RawCapture:
		if (!state.operationPending && state.selectedReady && state.selectedFrame == 4 && state.selectedLayers == 1)
		{
			m_rawCapture = state.selectedCapture;
			m_rawEpoch = state.selectedEpoch;
			m_rawFrame = state.selectedIdentity;
			for (const auto& frame : panels.TimelineFrames())
				if (frame.capture == m_rawCapture && frame.run.epoch == m_rawEpoch &&
					frame.run.session == m_rawFrame.session && frame.run.generation == m_rawFrame.run &&
					frame.frame == m_rawFrame.frame)
					m_rawLocal = frame.local;
			if (!m_rawLocal)
				throw std::runtime_error("Runtime smoke raw capture has no retained frame record");
			panels.Command(control::CommandKind::Step);
			m_stage = Stage::RetainedCapture;
		}
		break;
	case Stage::RetainedCapture:
		if (!state.operationPending && state.selectedReady && state.selectedFrame == 4 && state.selectedLayers == 2)
		{
			for (const auto capture : panels.CaptureIds())
				panels.Command(control::CommandKind::Release, capture);
			m_stage = Stage::Released;
		}
		break;
	case Stage::Released:
		if (!state.operationPending)
		{
			panels.SelectCapture(m_rawCapture);
			panels.Command(control::CommandKind::Resume);
			m_stage = Stage::Teardown;
		}
		break;
	case Stage::Teardown:
		if (!state.sessionAvailable && !state.pending)
		{
			// Finish teardown replies before selecting retained history in the empty session scope.
			panels.SelectFrame(m_rawLocal);
			m_stage = Stage::RetainedSelection;
		}
		break;
	case Stage::RetainedSelection:
		if (state.selectedEpoch != m_rawEpoch || state.selectedIdentity != m_rawFrame ||
			state.selectedImageCapture != m_rawCapture || !state.selectedReader || !state.selectedReady ||
			state.selectedLayers != 1 || state.images < 3)
		{
			std::fprintf(
				stderr,
				"Runtime smoke retained selection: local=%llu expected=%llu/%llu/%llu/%llu capture=%llu "
				"actual=%llu/%llu/%llu/%llu capture=%llu reader=%llu ready=%d layers=%u images=%zu\n",
				static_cast<unsigned long long>(m_rawLocal), static_cast<unsigned long long>(m_rawEpoch),
				static_cast<unsigned long long>(m_rawFrame.session), static_cast<unsigned long long>(m_rawFrame.run),
				static_cast<unsigned long long>(m_rawFrame.frame), static_cast<unsigned long long>(m_rawCapture),
				static_cast<unsigned long long>(state.selectedEpoch),
				static_cast<unsigned long long>(state.selectedIdentity.session),
				static_cast<unsigned long long>(state.selectedIdentity.run),
				static_cast<unsigned long long>(state.selectedIdentity.frame),
				static_cast<unsigned long long>(state.selectedImageCapture),
				static_cast<unsigned long long>(state.selectedReader), state.selectedReady, state.selectedLayers,
				state.images);
			throw std::runtime_error("Runtime smoke lost retained images after session teardown");
		}
		if (!m_exportPath.empty() && !panels.RequestExport(m_exportPath))
			throw std::runtime_error("Runtime smoke export was not admitted");
		m_holdUntil = renderer.SubmittedFrames() + 4;
		m_stage = Stage::Export;
		break;
	case Stage::Export:
		if (!m_exportPath.empty())
		{
			const auto& exportTask = panels.ImageExport();
			if (exportTask.Busy())
				break;
			if (!exportTask.Succeeded())
				throw std::runtime_error("Runtime smoke export failed: " + exportTask.Detail());
		}
		if (renderer.CompletedFrames() >= m_holdUntil)
			m_stage = Stage::Complete;
		break;
	case Stage::Complete:
		break;
	}
}
} // namespace simxr::app
