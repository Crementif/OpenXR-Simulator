#include "app/widgets/RecordingPanel.h"
#include "Pch.h"
#include "app/services/CompanionSettings.h"
#include "app/services/Utf8Path.h"
#include "app/widgets/Theme.h"
#include "platform/ProcessIdentity.h"

#include <chrono>
#include <ctime>
#include <imgui.h>
#include <stdexcept>

namespace simxr::app
{
RecordingPanel::RecordingPanel()
{
	try
	{
		m_directory = CompanionSettingsPath().parent_path() / "recordings";
	}
	catch (const std::exception& error)
	{
		m_error = error.what();
	}
}
void RecordingPanel::SetTarget(RecordingTarget target)
{
	if (target == m_target)
		return;
	m_retired |= m_wasBusy || m_capture.Snapshot().busy;
	m_saved.reset();
	m_capture.SetTarget(target);
	m_target = std::move(target);
	m_startRequested = m_stopRequested = false;
}
RecordingCaptureSnapshot RecordingPanel::Snapshot() const
{
	auto state = m_capture.Snapshot();
	if (!m_error.empty())
		state.error = m_error;
	return state;
}
void RecordingPanel::Poll(bool supported)
{
	if (std::exchange(m_stopRequested, false))
		m_capture.Stop();
	if (std::exchange(m_startRequested, false) && !m_capture.Snapshot().busy)
	{
		try
		{
			if (!supported)
				throw std::runtime_error("This runtime does not support sustained input recording");
			if (m_directory.empty())
				throw std::runtime_error("The recordings folder is unavailable");
			const auto now = std::chrono::system_clock::now();
			const auto time = std::chrono::system_clock::to_time_t(now);
			std::array<char, 64> name{};
			if (const auto local = std::localtime(&time))
				std::strftime(name.data(), name.size(), "Take %Y-%m-%d %H-%M-%S", local);
			const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
			if (m_capture.Start(m_directory / Utf8Path(std::string(name.data()) + "-" +
																   std::to_string(1000 + stamp % 1000).substr(1) + "-" +
																   std::to_string(AllocateProcessIdentity()) + ".simxrtake")))
			{
				m_error.clear();
				m_retired = false;
				m_showResult = false;
				m_wasBusy = true;
			}
		}
		catch (const std::exception& error)
		{
			m_error = error.what();
		}
	}
	const auto state = Snapshot();
	if (m_wasBusy && !state.busy)
	{
		m_wasBusy = false;
		m_showResult = true;
		if (state.phase == RecordingCapturePhase::Complete && !m_retired)
			m_saved = PathText(state.directory);
	}
}
void RecordingPanel::DrawPopovers(bool supported)
{
	(void)supported;
	if (std::exchange(m_showResult, false))
		ImGui::OpenPopup("Recorded take");
	if (!ImGui::BeginPopup("Recorded take"))
		return;
	const auto state = Snapshot();
	const auto width = 360 * ImGui::GetFontSize() / 17;
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
	ImGui::TextUnformatted(state.phase == RecordingCapturePhase::Complete ? "Take saved" : "Take incomplete");
	ImGui::TextDisabled("%llu input change%s saved", static_cast<unsigned long long>(state.transactions),
						state.transactions == 1 ? "" : "s");
	if (!state.directory.empty())
		ImGui::TextUnformatted(PathText(state.directory).c_str());
	if (!state.error.empty())
		ImGui::TextColored(theme::Vec(theme::Record), "%s", state.error.c_str());
	if (state.phase == RecordingCapturePhase::Complete)
		ImGui::TextUnformatted("Open Library to replay or rename this take.");
	ImGui::PopTextWrapPos();
	if (ImGui::Button("Done"))
		ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}
} // namespace simxr::app
