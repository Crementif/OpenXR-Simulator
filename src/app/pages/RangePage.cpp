#include "app/pages/RangePage.h"
#include "app/UiPch.h"
#include "app/services/CompanionSettings.h"
#include "app/services/RecordingController.h"
#include "app/services/RuntimeSession.h"
#include "app/services/Utf8Path.h"
#include "app/widgets/Controls.h"

#include <ctime>

namespace simxr::app
{
void RangePage::SaveTake(const RangeSel& range, std::size_t first, std::size_t last, ShellState& shell, bool replace)
{
	try
	{
		const std::string name = m_take.data();
		if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos)
			throw std::runtime_error("Give the take a name without folder separators or a colon");
		const auto directory = CompanionSettingsPath().parent_path() / "recordings";
		const auto path = directory / Utf8Path(name + ".luau");
		if (!replace && std::filesystem::exists(path))
		{
			m_replace = true;
			m_takeStatus = "A take with this name exists. Replace it or choose another name.";
			return;
		}
		const auto& history = m_session.History();
		std::vector<const InputSample*> samples;
		for (auto index = first; index <= last; ++index)
			if (const auto* sample = m_session.InputSamples().Find(history.At(index).input))
				samples.push_back(sample);
			else
				throw std::runtime_error("Some input is missing. Select frames with saved input.");
		const auto take = TakeFromInput(samples);
		std::filesystem::create_directories(directory);
		SaveRecording(path, take, replace);
		m_replace = false;
		m_last = automation::SavedRecording{PathText(path), take.seed};
		shell.timeline.savedTake = TakeSource{m_last->path, range.first, range.last, range.epoch};
		m_takeStatus = "Saved " + std::to_string(take.total) + " changes";
		m_session.Notify({"Take saved", name, "Play", [this, saved = *m_last]
						  {
							  m_play = saved;
						  }});
	}
	catch (const std::exception& error)
	{
		m_last.reset();
		m_takeStatus = error.what();
	}
}
std::optional<automation::SavedRecording> RangePage::Draw(const RangeSel& range, ShellState& shell)
{
	if (!m_range || m_range->first != range.first || m_range->last != range.last || m_range->epoch != range.epoch)
	{
		m_range = range;
		m_take = {};
		m_last.reset();
		m_replace = false;
		m_takeStatus.clear();
	}
	const auto& history = m_session.History();
	const auto first = history.Find(range.first, range.epoch), last = history.Find(range.last, range.epoch);
	const std::string title = "Frames " + std::to_string(range.first.frame) + " to " + std::to_string(range.last.frame);
	PageTitle(Icon::Crop, title.c_str());
	if (PageClose())
		shell.selection = std::monostate{};
	if (!first || !last || *last < *first)
	{
		ImGui::TextDisabled("These frames are no longer in history");
		return {};
	}
	ImGui::Text("%.3f seconds / %zu frames", history.At(*last).seconds - history.At(*first).seconds,
				*last - *first + 1);
	if (ImGui::Button("Zoom to range"))
	{
		shell.timeline.first = history.At(*first).seconds;
		shell.timeline.last = history.At(*last).seconds;
		shell.timeline.zoomToRange = true;
	}

	SectionLabel("Take");
	const bool recorded = *last > *first && m_session.InputSamples().Find(history.At(*first).input) &&
						  m_session.InputSamples().Find(history.At(*last).input);
	if (!m_take[0])
	{
		const auto now = std::time(nullptr);
		if (const auto local = std::localtime(&now))
			std::strftime(m_take.data(), m_take.size(), "Cut %Y-%m-%d %H-%M-%S", local);
	}
	ImGui::SetNextItemWidth(-1);
	const auto takeId = std::to_string(range.epoch) + "/" + std::to_string(range.first.session) + "/" +
						std::to_string(range.first.run) + "/" + std::to_string(range.first.frame) + "/" +
						std::to_string(range.last.frame);
	ImGui::PushID(takeId.c_str());
	if (ImGui::InputText("##Take name", m_take.data(), m_take.size()))
	{
		m_replace = false;
		m_last.reset();
		m_takeStatus.clear();
	}
	ImGui::PopID();
	ImGui::BeginDisabled(!recorded);
	if (ImGui::Button(m_replace ? "Replace take" : "Save as take"))
		SaveTake(range, *first, *last, shell, m_replace);
	ImGui::EndDisabled();
	ImGui::SetItemTooltip(recorded			? "Saves the device input of these frames as a take"
						  : *last == *first ? "Select at least two frames with input"
											: "No input was recorded for these frames");
	std::optional<automation::SavedRecording> replay;
	if (m_last)
	{
		ImGui::SameLine();
		ImGui::BeginDisabled(!m_session.Connected());
		if (ImGui::Button("Play"))
			replay = m_last;
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("Play %s on the app", PathText(Utf8Path(m_last->path).stem()).c_str());
	}
	if (!m_takeStatus.empty())
	{
		ImGui::PushTextWrapPos();
		ImGui::TextDisabled("%s", m_takeStatus.c_str());
		ImGui::PopTextWrapPos();
	}

	SectionLabel("Video");
	ImGui::SetNextItemWidth(-1);
	ImGui::InputText("##Video file", m_videoPath.data(), m_videoPath.size());
	ImGui::BeginDisabled(m_export.Busy() || m_session.Exports().Busy() || !m_session.Connected() ||
						 !m_session.Supports(control::Capability::VideoExport) || range.epoch != m_session.Epoch());
	if (ImGui::Button("Save video"))
	{
		try
		{
			m_session.Exports().RequestVideo(range.first.session, range.first.run, range.first.frame, range.last.frame,
											 m_videoPath.data());
		}
		catch (const std::exception& error)
		{
			m_takeStatus = error.what();
		}
	}
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Saves both preview eyes as an MP4 without audio. Replaces an existing file.");
	if (!m_session.Exports().Detail().empty())
		ImGui::TextWrapped("%s", m_session.Exports().Detail().c_str());

	SectionLabel("Export");
	ImGui::SetNextItemWidth(-1);
	ImGui::InputText("##Range export file", m_path.data(), m_path.size());
	ImGui::BeginDisabled(m_export.Busy() || m_session.Exports().Busy());
	if (ImGui::Button("Export frames"))
		m_export.Start(range.first, range.last, range.epoch, m_path.data());
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Saves frame timing as CSV and kept snapshots as PPM next to it");
	if (!m_export.Metadata().empty())
		ImGui::TextWrapped("%s", m_export.Metadata().c_str());
	if (!m_export.Images().empty())
		ImGui::TextWrapped("%s", m_export.Images().c_str());
	return replay;
}
} // namespace simxr::app
