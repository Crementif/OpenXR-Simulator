#include "platform/linux/LinuxRuntimeStatus.h"
#include "Pch.h"

#include "control/legacy/LegacyFileChannel.h"
#include "control/legacy/LegacyInput.h"
#include "runtime/Session.h"

#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace simxr
{
LinuxRuntimeStatus::LinuxRuntimeStatus(std::shared_ptr<LegacyFileChannel> files, int processId, ExecutionMode mode)
	: m_files(std::move(files)), m_processId(processId), m_mode(mode)
{
	if (!m_files)
		throw std::invalid_argument("Runtime status requires its instance file channel");
}

void LinuxRuntimeStatus::Update(const SessionSnapshot& session, const LegacyInput& input, Clock::time_point now,
								std::optional<FrameCounts> submitted) noexcept
{
	try
	{
		if (m_generation != session.generation)
		{
			m_timing = {};
			m_lastPublication.reset();
			m_generation = session.generation;
		}
		if (submitted)
			m_timing.Observe(submitted->projections != 0, now);
		if (session.running && m_mode != ExecutionMode::Deterministic && m_lastPublication &&
			now - *m_lastPublication < std::chrono::milliseconds(500))
			return;
		const auto& head = input.HeadPose();
		const auto angles = input.HeadAngles();
		std::ostringstream runtimeStatus;
		runtimeStatus.imbue(std::locale::classic());
		runtimeStatus << "{\"runtime\":\"OpenXR Simulator Linux Vulkan\",\"pid\":" << m_processId
					  << ",\"session_state\":\"" << (session.running ? "FOCUSED" : "IDLE")
					  << "\",\"frame_count\":" << session.lastSubmitted.value
					  << ",\"head_tracking\":{\"position\":{\"x\":" << head.position.x << ",\"y\":" << head.position.y
					  << ",\"z\":" << head.position.z << "},\"yaw\":" << angles.yaw << ",\"pitch\":" << angles.pitch
					  << ",\"roll\":" << angles.roll << "}}";
		const auto performance = m_timing.Snapshot(now);
		const auto counts = submitted.value_or(FrameCounts{});
		std::ostringstream frameStatus;
		frameStatus.imbue(std::locale::classic());
		frameStatus << "{\"frame_count\":" << session.lastSubmitted.value << ",\"layer_count\":" << counts.layers
					<< ",\"projection_layers\":" << counts.projections
					<< ",\"predicted_display_time\":" << session.lastIssued.displayTime.value.count()
					<< ",\"fps\":" << std::fixed << std::setprecision(6) << performance.fps
					<< ",\"graphics_api\":\"Vulkan\"}";
		m_files->Publish(LegacyOutputFile::RuntimeStatus, runtimeStatus.str());
		m_files->Publish(LegacyOutputFile::FrameInfo, frameStatus.str());
		m_lastPublication = now;
	}
	catch (...)
	{
		try
		{
			m_files->Log("Runtime status publication failed");
		}
		catch (...)
		{
		}
	}
}
} // namespace simxr
