#include "app/RangePage.h"
#include "app/RuntimeSession.h"
#include "app/CompanionSettings.h"
#include "app/Utf8Path.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <imgui.h>

namespace simxr::app
{
RangeExportTask::Protection::Protection(PreviewImages& preview, std::span<const std::uint64_t> images)
	: m_preview(preview)
{
	m_preview.ProtectExports(images);
}
RangeExportTask::Protection::~Protection()
{
	m_preview.ProtectExports({});
}
bool RangeExportTask::Busy() const
{
	return m_csv.valid() || m_protection.has_value();
}
void RangeExportTask::Finish(std::string result)
{
	m_images = std::move(result);
	m_queue.clear();
	m_protection.reset();
}
void RangeExportTask::Start(const RangeSel& range, std::size_t first, std::size_t last, const std::string& path)
{
	const auto& history = m_session.History();
	std::vector<HistoryFrame> frames;
	frames.reserve(last - first + 1);
	for (auto index = first; index <= last; ++index)
		frames.push_back(history.At(index));
	m_metadata.clear();
	m_images.clear();
	m_queue.clear();
	const auto prefix = std::filesystem::path(path).replace_extension().string();
	std::vector<std::uint64_t> images;
	for (const auto& image : m_session.Preview().Images())
		if (!image.live && image.ready && !image.released && image.reader && image.scope.epoch == range.epoch &&
			image.scope.epoch == m_session.Epoch() && image.scope.session == range.first.session &&
			image.scope.generation == range.first.run && image.frame >= range.first.frame &&
			image.frame <= range.last.frame)
		{
			images.push_back(image.id);
			m_queue.push_back({image.reader, prefix + "-frame-" + std::to_string(image.frame) + "-image-" +
												 std::to_string(image.id) + ".ppm"});
		}
	m_epoch = m_session.Epoch();
	m_next = 0;
	m_requested = false;
	if (m_queue.empty())
		m_images = "No snapshots kept in this range";
	else
		m_protection.emplace(m_session.Preview(), images);
	m_csv = std::async(std::launch::async,
					   [frames = std::move(frames), path]
					   {
						   std::ofstream output(path);
						   output << "epoch,session,run,frame,run_seconds,display_interval_ms,snapshot,preview,"
									 "dropped\n";
						   for (const auto& frame : frames)
							   output << frame.epoch << ',' << frame.identity.session << ',' << frame.identity.run
									  << ',' << frame.identity.frame << ',' << frame.runSeconds << ','
									  << frame.frameMilliseconds << ',' << bool(frame.flags & HistorySnapshot) << ','
									  << bool(frame.flags & HistoryPreview) << ',' << bool(frame.flags & HistoryDropped)
									  << '\n';
						   output.close();
						   return output ? "Saved frame metadata: " + path : "Could not save frame metadata: " + path;
					   });
}
void RangeExportTask::Poll()
{
	if (m_csv.valid() && m_csv.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		m_metadata = m_csv.get();
	if (!m_protection)
		return;
	if (m_epoch != m_session.Epoch() || !m_session.Connected())
	{
		Finish("Snapshot export stopped: connection changed");
		return;
	}
	auto& exports = m_session.Exports();
	if (exports.Busy())
		return;
	if (m_requested)
	{
		if (!exports.Succeeded())
		{
			Finish(exports.Detail());
			return;
		}
		++m_next;
		m_requested = false;
	}
	if (m_next == m_queue.size())
	{
		Finish("Saved " + std::to_string(m_next) + " snapshots");
		return;
	}
	const auto& [reader, file] = m_queue[m_next];
	m_requested = exports.Request(reader, file);
	if (!m_requested)
		Finish("Snapshot export could not start");
}
void RangePage::SaveTake(const RangeSel& range, std::size_t first, std::size_t last, ShellState& shell)
{
	try
	{
		const std::string name = m_take.data();
		if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos)
			throw std::runtime_error("Give the take a name without folder separators or a colon");
		if (!m_session.Setup())
			throw std::runtime_error("Device setup is not known yet");
		const auto& history = m_session.History();
		std::vector<const InputSample*> samples;
		for (auto index = first; index <= last; ++index)
			if (const auto* sample = m_session.InputSamples().Find(history.At(index).input))
				samples.push_back(sample);
		const auto take = TakeFromInput(samples, m_session.Setup()->values);
		const auto script = automation::EditableRig(take);
		const auto directory = CompanionSettingsPath().parent_path() / "recordings";
		std::filesystem::create_directories(directory);
		const auto path = directory / Utf8Path(name + ".luau");
		const auto temporary = std::filesystem::path(path).concat(".writing");
		{
			std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
			output.write(script.data(), static_cast<std::streamsize>(script.size()));
			if (!output.flush())
				throw std::runtime_error("Cannot write the take");
		}
		std::filesystem::rename(temporary, path);
		const auto text = path.u8string();
		m_last = automation::SavedRecording{{text.begin(), text.end()}, take.seed};
		shell.timeline.savedTake = TakeSource{m_last->path, range.first, range.last, range.epoch};
		m_takeStatus = "Saved " + std::to_string(take.total) + " changes";
	}
	catch (const std::exception& error)
	{
		m_last.reset();
		m_takeStatus = error.what();
	}
}
std::optional<automation::SavedRecording> RangePage::Draw(const RangeSel& range, ShellState& shell)
{
	const auto& history = m_session.History();
	const auto first = history.Find(range.first, range.epoch), last = history.Find(range.last, range.epoch);
	ImGui::SeparatorText("Range");
	ImGui::Text("Frames %llu – %llu", static_cast<unsigned long long>(range.first.frame),
				static_cast<unsigned long long>(range.last.frame));
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

	ImGui::SeparatorText("Take");
	const bool recorded = m_session.InputSamples().Find(history.At(*first).input) && m_session.InputSamples().Find(history.At(*last).input);
	if (!m_take[0])
	{
		const auto now = std::time(nullptr);
		std::strftime(m_take.data(), m_take.size(), "Cut %Y-%m-%d %H-%M-%S", std::localtime(&now));
	}
	ImGui::SetNextItemWidth(-1);
	ImGui::InputText("##Take name", m_take.data(), m_take.size());
	ImGui::BeginDisabled(!recorded);
	if (ImGui::Button("Save as take"))
		SaveTake(range, *first, *last, shell);
	ImGui::EndDisabled();
	ImGui::SetItemTooltip(recorded ? "Saves the head and hand input of these frames as a replayable take"
								   : "No input was recorded for these frames");
	std::optional<automation::SavedRecording> replay;
	if (m_last)
	{
		ImGui::SameLine();
		ImGui::BeginDisabled(!m_session.Connected());
		if (ImGui::Button("Replay"))
			replay = m_last;
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("Replay %s", Utf8Path(m_last->path).stem().string().c_str());
	}
	if (!m_takeStatus.empty())
		ImGui::TextDisabled("%s", m_takeStatus.c_str());

	ImGui::SeparatorText("Export");
	ImGui::SetNextItemWidth(-1);
	ImGui::InputText("##Range export file", m_path.data(), m_path.size());
	ImGui::BeginDisabled(m_export.Busy() || m_session.Exports().Busy());
	if (ImGui::Button("Export frames"))
		m_export.Start(range, *first, *last, m_path.data());
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Saves frame timing as CSV and kept snapshots as PPM next to it");
	if (!m_export.Metadata().empty())
		ImGui::TextWrapped("%s", m_export.Metadata().c_str());
	if (!m_export.Images().empty())
		ImGui::TextWrapped("%s", m_export.Images().c_str());
	return replay;
}
} // namespace simxr::app
