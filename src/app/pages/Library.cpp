#include "app/pages/Library.h"
#include "app/UiPch.h"
#include "app/RuntimePanels.h"
#include "app/services/CompanionSettings.h"
#include "app/services/Utf8Path.h"
#include "app/widgets/Controls.h"
#include "app/widgets/Theme.h"

#include <cctype>
#include <imgui_internal.h>
#include <tuple>

namespace simxr::app
{
static bool Contains(const std::string& text, const char* search)
{
	const std::string_view needle = search;
	return std::ranges::search(
			   text, needle,
			   [](char a, char b)
			   {
				   return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			   }).begin() != text.end() ||
		   needle.empty();
}
static std::string Age(std::filesystem::file_time_type modified)
{
	const auto now = std::chrono::duration_cast<std::chrono::seconds>(
		std::filesystem::file_time_type::clock::now().time_since_epoch());
	const auto saved = std::chrono::duration_cast<std::chrono::seconds>(modified.time_since_epoch());
	const auto seconds = (now - saved).count();
	char text[32];
	if (seconds < 60)
		return "now";
	if (seconds < 3600)
		std::snprintf(text, sizeof(text), "%lld min", static_cast<long long>(seconds / 60));
	else if (seconds < 86400)
		std::snprintf(text, sizeof(text), "%lld h", static_cast<long long>(seconds / 3600));
	else
		std::snprintf(text, sizeof(text), "%lld d", static_cast<long long>(seconds / 86400));
	return text;
}
static const char* PhaseName(automation::RigPhase phase)
{
	switch (phase)
	{
	case automation::RigPhase::Idle:
		return "Idle";
	case automation::RigPhase::Running:
		return "Running";
	case automation::RigPhase::Paused:
		return "Paused";
	case automation::RigPhase::Complete:
		return "Complete";
	case automation::RigPhase::Cancelled:
		return "Cancelled";
	case automation::RigPhase::Failed:
		return "Failed";
	}
	return "Unknown";
}
Library::Library()
{
	try
	{
		const auto root = CompanionSettingsPath().parent_path();
		m_takes = root / "recordings";
		m_scripts = root / "scripts";
		m_catalog.emplace(root);
	}
	catch (const std::exception& error)
	{
		m_error = error.what();
	}
}
void Library::Refresh()
{
	if (m_catalog)
		m_catalog->Refresh();
	m_nextScan = std::chrono::steady_clock::now() + std::chrono::seconds(3);
}
void Library::Advance(Selection& selection, RigController& rigs)
{
	if (auto report = rigs.TakeReportResult())
	{
		m_error.clear();
		m_report.clear();
		if (!report->error)
			m_report = std::move(report->path);
		else
			try
			{
				std::rethrow_exception(report->error);
			}
			catch (const std::exception& error)
			{
				m_error = error.what();
			}
			catch (...)
			{
				m_error = "Cannot save rig report";
			}
	}
	if (m_catalog)
		if (auto result = m_catalog->TakeResult())
		{
			if (result->evidence && result->path == m_rangePath)
			{
				m_evidence = std::move(result->evidence);
				m_evidenceSelected = 0;
			}
			if (result->entries)
			{
				m_entries = std::move(*result->entries);
				m_limited = result->limited;
			}
			if (result->changed)
			{
				if (auto take = std::get_if<TakeSel>(&selection); take && take->id == result->path)
					selection = result->action == LibraryAction::Delete ? Selection{} : TakeSel{result->renamed};
				else if (auto script = std::get_if<ScriptSel>(&selection); script && script->id == result->path)
					selection = result->action == LibraryAction::Delete ? Selection{} : ScriptSel{result->renamed};
			}
			auto& detail = result->action == LibraryAction::Scan ? m_scanError : m_error;
			detail.clear();
			if (result->error)
				try
				{
					std::rethrow_exception(result->error);
				}
				catch (const std::exception& error)
				{
					detail = error.what();
				}
				catch (...)
				{
					detail = "Library file operation failed";
				}
		}
}
bool Library::Busy() const
{
	return m_catalog && m_catalog->Busy();
}
void Library::Poll(Selection& selection, RigController& rigs)
{
	Advance(selection, rigs);
	if (std::chrono::steady_clock::now() >= m_nextScan)
		Refresh();
}
const LibraryEntry* Library::Find(const std::string& path) const
{
	const auto found = std::ranges::find(m_entries, path, &LibraryEntry::path);
	return found == m_entries.end() ? nullptr : &*found;
}
void Library::Play(const LibraryEntry& entry, RuntimePanels& runtime)
{
	m_error.clear();
	if (entry.take)
		runtime.RequestReplay({entry.path, entry.seed});
	else
		m_run = ScriptRun{entry.path, m_options};
}
std::optional<ScriptRun> Library::Draw(Selection& selection, RuntimePanels& runtime, RigController& rigs)
{
	Poll(selection, rigs);
	if (const auto take = std::get_if<TakeSel>(&selection))
		DrawTake(selection, std::string(take->id), runtime, rigs);
	else if (const auto script = std::get_if<ScriptSel>(&selection))
		DrawScript(selection, std::string(script->id), runtime, rigs);
	return std::exchange(m_run, {});
}
std::optional<ScriptRun> Library::DrawBrowser(Selection& selection, RuntimePanels& runtime, RigController& rigs)
{
	Poll(selection, rigs);
	const float unit = ImGui::GetFontSize() / 17;
	ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
	ImGui::InputTextWithHint("##Search library", "Search", m_search.data(), m_search.size());
	ImGui::SameLine();
	if (IconButton("Refresh library", Icon::Refresh, "Rescan takes and scripts"))
		Refresh();
	const bool idle = runtime.CanReplay() && !rigs.IsActive() && !rigs.WaitingForLaunch();
	if (m_catalog && m_catalog->Busy())
		ImGui::TextDisabled("Updating library…");
	if (m_limited)
		ImGui::TextDisabled("Showing 512 entries. Open other scripts by path.");
	if (m_entries.empty())
	{
		ImGui::TextDisabled("No takes or scripts");
		ImGui::SetItemTooltip("Takes: %s\nScripts: %s", PathText(m_takes).c_str(), PathText(m_scripts).c_str());
	}
	else if (ImGui::BeginTable("Library entries", 3,
							   ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX,
							   {0, std::max(ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2,
											ImGui::GetFrameHeight() * 4)}))
	{
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 48 * unit);
		ImGui::TableSetupColumn("Play", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
		for (const auto& entry : m_entries)
		{
			if (!Contains(entry.name, m_search.data()))
				continue;
			ImGui::PushID(entry.path.c_str());
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			const auto position = ImGui::GetCursorScreenPos();
			const float icon = ImGui::GetFontSize();
			DrawIcon(ImGui::GetWindowDrawList(), entry.take ? Icon::Record : Icon::Script,
					 {position.x + icon / 2, position.y + ImGui::GetFrameHeight() / 2}, icon,
					 entry.take ? theme::Record : theme::TextDim);
			ImGui::SetCursorScreenPos({position.x + icon + 4 * unit, position.y});
			ImGui::AlignTextToFramePadding();
			const auto labelOrigin = ImGui::GetCursorScreenPos();
			const auto labelRight = labelOrigin.x + ImGui::GetContentRegionAvail().x;
			if (ImGui::Selectable("##Entry", false,
								  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
			{
				if (entry.take)
					selection = TakeSel{entry.path};
				else
					selection = ScriptSel{entry.path};
			}
			ImGui::RenderTextEllipsis(
				ImGui::GetWindowDrawList(),
				{labelOrigin.x, position.y + (ImGui::GetFrameHeight() - ImGui::GetFontSize()) / 2},
				{labelRight, position.y + ImGui::GetFrameHeight()}, labelRight, entry.name.c_str(), nullptr, nullptr);
			ImGui::SetItemTooltip("%s%s%s", entry.path.c_str(), entry.error.empty() ? "" : "\n", entry.error.c_str());
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%s", Age(entry.modified).c_str());
			ImGui::TableNextColumn();
			ImGui::BeginDisabled(!idle || !entry.error.empty());
			if (IconButton("Play entry", Icon::Play,
						   entry.take ? "Replay recorded inputs. This does not restore game state."
									  : "Run this script on the selected app"))
				Play(entry, runtime);
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Open").x - ImGui::GetStyle().FramePadding.x * 2 -
							ImGui::GetStyle().ItemSpacing.x);
	ImGui::InputTextWithHint("##Open script", "Script path (.luau)", m_open.data(), m_open.size());
	ImGui::SameLine();
	ImGui::BeginDisabled(!m_open[0]);
	if (ImGui::Button("Open"))
		selection = ScriptSel{m_open.data()};
	ImGui::EndDisabled();
	for (const auto* error : {&m_error, &m_scanError})
		if (!error->empty())
			ImGui::TextColored(theme::Vec(theme::Record), "%s", error->c_str());
	return std::exchange(m_run, {});
}
bool Library::DrawHeader(Selection& selection, const std::string& path, Icon kind, bool changing)
{
	const auto name = PathText(Utf8Path(path).stem());
	const float button = ImGui::GetFrameHeight(), spacing = ImGui::GetStyle().ItemSpacing.x;
	const auto origin = ImGui::GetCursorScreenPos();
	const auto width = std::max(button, ImGui::GetContentRegionAvail().x - button * 3 - spacing * 3);
	DrawIcon(ImGui::GetWindowDrawList(), kind, {origin.x + button / 2, origin.y + button / 2}, button * .7f,
			 theme::Text);
	ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(),
							  {origin.x + button, origin.y + (button - ImGui::GetFontSize()) / 2},
							  {origin.x + width, origin.y + button}, origin.x + width, name.c_str(), nullptr, nullptr);
	ImGui::Dummy({width, button});
	ImGui::SetItemTooltip("%s", path.c_str());
	ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - button * 3 - spacing * 2));
	ImGui::BeginDisabled(!changing || !m_catalog || m_catalog->Busy());
	if (IconButton("Rename entry", Icon::Rename, "Change this file name on disk"))
	{
		std::snprintf(m_rename.data(), m_rename.size(), "%s", name.c_str());
		ImGui::OpenPopup("Rename");
	}
	ImGui::SameLine();
	if (IconButton("Delete entry", Icon::Delete, "Delete this file after confirmation.", false, nullptr, theme::Record))
		ImGui::OpenPopup("Delete?");
	ImGui::EndDisabled();
	if (PageClose())
	{
		selection = std::monostate{};
		return false;
	}
	if (ImGui::BeginPopup("Rename"))
	{
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		const bool entered =
			ImGui::InputText("##New name", m_rename.data(), m_rename.size(), ImGuiInputTextFlags_EnterReturnsTrue);
		const std::string next = m_rename.data();
		const bool valid = LibraryCatalog::ValidName(next);
		ImGui::BeginDisabled(!valid || !changing || !m_catalog || m_catalog->Busy());
		if (ImGui::Button("Rename") || (entered && valid && changing && m_catalog && !m_catalog->Busy()))
		{
			if (m_catalog->Rename(path, next))
				ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::EndPopup();
	}
	if (ImGui::BeginPopup("Delete?"))
	{
		ImGui::Text("Delete %s?", name.c_str());
		ImGui::BeginDisabled(!changing || !m_catalog || m_catalog->Busy());
		if (ImGui::Button("Delete") && m_catalog->Delete(path))
			ImGui::CloseCurrentPopup();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Keep"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	for (const auto* error : {&m_error, &m_scanError})
		if (!error->empty())
			ImGui::TextColored(theme::Vec(theme::Record), "%s", error->c_str());
	return true;
}
void Library::DrawTake(Selection& selection, const std::string& path, RuntimePanels& runtime, RigController& rigs)
{
	if (!DrawHeader(selection, path, Icon::Record,
					!runtime.View().recording && !runtime.View().recordingBusy && !rigs.IsActive()))
		return;
	const auto* entry = Find(path);
	if (!entry)
	{
		ImGui::TextDisabled(m_catalog && m_catalog->Busy() ? "Loading take…" : "Take not found");
		return;
	}
	if (!entry->error.empty())
	{
		ImGui::TextColored(theme::Vec(theme::Record), "%s", entry->error.c_str());
		if (!entry->archive)
			return;
	}
	ImGui::Separator();
	const bool mine = rigs.ReplayingTake() && rigs.Script() == path;
	if (mine && rigs.IsActive())
	{
		if (IconButton("Stop take", Icon::Stop, "Stop playing and release its inputs", false, "Stop"))
			rigs.Cancel();
	}
	else
	{
		ImGui::BeginDisabled(!entry->error.empty() || !runtime.CanReplay() || rigs.IsActive() ||
							 rigs.WaitingForLaunch());
		if (IconButton("Play take", Icon::Play, "Play this take on the selected session", false, "Play"))
			Play(*entry, runtime);
		ImGui::EndDisabled();
	}
	if (mine)
	{
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", rigs.Error().empty() ? PhaseName(rigs.Snapshot().phase) : "Failed");
	}
	if (ImGui::BeginTable("Take details", 2))
	{
		ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 70 * ImGui::GetFontSize() / 17);
		const auto row = [](const char* key, const std::string& value)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%s", key);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(value.c_str());
		};
		row("Seed", std::to_string(entry->seed));
		row("Saved", Age(entry->modified) == "now" ? "just now" : Age(entry->modified) + " ago");
		row("Size", std::to_string(entry->bytes / 1024 + (entry->bytes % 1024 != 0)) + " KiB");
		ImGui::EndTable();
	}
	if (entry->archive)
	{
		if (m_rangePath != path)
		{
			m_rangePath = path;
			m_evidence.reset();
			m_evidenceRequested = false;
			m_evidenceModified = entry->modified;
			m_rangeFirst = entry->firstFrame;
			m_rangeLast = entry->lastFrame;
			std::snprintf(m_rangeName.data(), m_rangeName.size(), "Range %llu-%llu",
						  static_cast<unsigned long long>(m_rangeFirst), static_cast<unsigned long long>(m_rangeLast));
		}
		ImGui::Separator();
		ImGui::TextUnformatted("Editable range");
		ImGui::SetNextItemWidth(-100 * ImGui::GetFontSize() / 17);
		ImGui::InputScalar("First frame", ImGuiDataType_U64, &m_rangeFirst);
		ImGui::SetItemTooltip(
			"The recording origin or a frame with applied input. This frame becomes the initial rig state.");
		ImGui::SetNextItemWidth(-100 * ImGui::GetFontSize() / 17);
		ImGui::InputScalar("Last frame", ImGuiDataType_U64, &m_rangeLast);
		ImGui::SetItemTooltip("An inclusive frame with applied input. Select up to 1,024 subsequent input outcomes.");
		ImGui::SetNextItemWidth(-100 * ImGui::GetFontSize() / 17);
		ImGui::InputText("Take name", m_rangeName.data(), m_rangeName.size());
		ImGui::BeginDisabled(!entry->error.empty() || !m_catalog || m_catalog->Busy() || runtime.View().recording ||
							 rigs.IsActive() || m_rangeFirst < entry->firstFrame || m_rangeLast > entry->lastFrame ||
							 m_rangeLast < m_rangeFirst || !LibraryCatalog::ValidName(m_rangeName.data()));
		if (ImGui::Button("Export editable rig"))
			m_catalog->ExportRange(path, m_rangeName.data(), m_rangeFirst, m_rangeLast);
		ImGui::EndDisabled();
		DrawEvidence(*entry, !runtime.View().recording && !rigs.IsActive() && entry->error.empty());
	}
	if (mine && !rigs.Error().empty())
		ImGui::TextColored(theme::Vec(theme::Record), "%s", rigs.Error().c_str());
}
static const char* EvidenceKind(GameObservationKind kind)
{
	switch (kind)
	{
	case GameObservationKind::Registered:
		return "Game registered";
	case GameObservationKind::Telemetry:
		return "Telemetry";
	case GameObservationKind::Unregistered:
		return "Game disconnected";
	case GameObservationKind::Closed:
		return "Bridge closed";
	}
	return "Observation";
}
void Library::DrawEvidence(const LibraryEntry& entry, bool changing)
{
	if (m_evidenceModified != entry.modified)
	{
		m_evidenceModified = entry.modified;
		m_evidence.reset();
		m_evidenceRequested = false;
	}
	if (!m_evidenceRequested && m_catalog && !m_catalog->Busy())
		m_evidenceRequested = m_catalog->ReadEvidence(entry.path);
	ImGui::Separator();
	ImGui::TextUnformatted("SDK evidence");
	if (!m_evidence)
	{
		ImGui::TextDisabled(m_catalog && m_catalog->Busy() ? "Loading evidence…" : "Evidence unavailable");
		if (m_catalog && !m_catalog->Busy() && ImGui::Button("Retry evidence"))
			m_evidenceRequested = false;
		return;
	}
	const auto& evidence = *m_evidence;
	if (!evidence.available)
	{
		ImGui::TextDisabled("This take has no SDK journal");
		return;
	}
	if (!evidence.complete || evidence.gap)
		ImGui::TextColored(theme::Vec(theme::Record), "SDK evidence is incomplete");
	else if (!evidence.correlated)
		ImGui::TextDisabled("Saved events · frame timing unavailable");
	else
		ImGui::TextDisabled("Saved events · clock correlation available");
	if (ImGui::CollapsingHeader("Initial SDK state"))
	{
		ImGui::Text("Game %llu · revision %llu", static_cast<unsigned long long>(evidence.initial.game),
					static_cast<unsigned long long>(evidence.initial.revision));
		if (ImGui::BeginChild("SDK initial payload", {0, 80 * ImGui::GetFontSize() / 17}, ImGuiChildFlags_Borders))
			ImGui::TextWrapped("%s", evidence.initial.telemetry.empty() ? "No telemetry"
																		: evidence.initial.telemetry.c_str());
		ImGui::EndChild();
	}
	const bool busy = !m_catalog || m_catalog->Busy();
	ImGui::BeginDisabled(busy);
	if (ImGui::Button("First events"))
		m_catalog->ReadEvidence(entry.path, evidence.first);
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(busy || !evidence.more);
	if (ImGui::Button("Next events"))
		m_catalog->ReadEvidence(entry.path, evidence.next);
	ImGui::EndDisabled();
	if (evidence.entries.empty())
	{
		ImGui::TextDisabled("No events in this capture interval");
		return;
	}
	if (ImGui::BeginChild("SDK events", {0, 100 * ImGui::GetFontSize() / 17}, ImGuiChildFlags_Borders))
		for (std::size_t index = 0; index < evidence.entries.size(); ++index)
		{
			const auto& event = evidence.entries[index].observation;
			const auto label = std::to_string(event.sequence) + "  " + EvidenceKind(event.kind);
			if (ImGui::Selectable(label.c_str(), m_evidenceSelected == index))
				m_evidenceSelected = index;
		}
	ImGui::EndChild();
	const auto& selected = evidence.entries[std::min(m_evidenceSelected, evidence.entries.size() - 1)];
	ImGui::Text("Game %llu · revision %llu", static_cast<unsigned long long>(selected.observation.game),
				static_cast<unsigned long long>(selected.observation.revision));
	if (ImGui::BeginChild("SDK payload", {0, 80 * ImGui::GetFontSize() / 17}, ImGuiChildFlags_Borders))
		ImGui::TextWrapped("%s",
						   selected.observation.payload.empty() ? "No payload" : selected.observation.payload.c_str());
	ImGui::EndChild();
	const bool range = selected.first && selected.last;
	if (range)
		ImGui::Text("Near frames %llu–%llu", static_cast<unsigned long long>(selected.first->value),
					static_cast<unsigned long long>(selected.last->value));
	else
		ImGui::TextDisabled("No reliable recorded frame range");
	ImGui::BeginDisabled(!range || !changing);
	if (ImGui::Button("Use nearby range"))
	{
		m_rangeFirst = selected.first->value;
		m_rangeLast = selected.last->value;
		std::snprintf(m_rangeName.data(), m_rangeName.size(), "Range %llu-%llu",
					  static_cast<unsigned long long>(m_rangeFirst), static_cast<unsigned long long>(m_rangeLast));
	}
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Selects recorded input boundaries around the timestamp uncertainty. This does not identify "
						  "the game frame that produced the event.");
}
void Library::DrawScript(Selection& selection, const std::string& path, RuntimePanels& runtime, RigController& rigs)
{
	if (!DrawHeader(selection, path, Icon::Script))
		return;
	ImGui::Separator();
	const float unit = ImGui::GetFontSize() / 17;
	const bool mine = !rigs.ReplayingTake() && rigs.Script() == path;
	const bool active = mine && rigs.IsActive();
	const auto state = mine ? rigs.Snapshot() : automation::RigSnapshot{};
	ImGui::BeginDisabled(active);
	if (ImGui::BeginTable("Script options", 2))
	{
		ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 70 * unit);
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Seed");
		ImGui::TableNextColumn();
		ImGui::SetNextItemWidth(-1);
		ImGui::InputScalar("##Seed", ImGuiDataType_U64, &m_options.seed);
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Deadline");
		ImGui::TableNextColumn();
		ImGui::SetNextItemWidth(-1);
		ImGui::SliderInt("##Deadline", &m_options.deadlineSeconds, 1, 3600, "%d s", ImGuiSliderFlags_Logarithmic);
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Memory");
		ImGui::TableNextColumn();
		ImGui::SetNextItemWidth(-1);
		ImGui::SliderInt("##Memory", &m_options.memoryMiB, 1, 512, "%d MiB", ImGuiSliderFlags_Logarithmic);
		ImGui::EndTable();
	}
	ImGui::EndDisabled();
	if (active)
	{
		const bool paused = state.phase == automation::RigPhase::Paused;
		if (paused ? IconButton("Resume script", Icon::Play, "Continue this script.")
				   : IconButton("Pause script", Icon::Pause, "Pause before the next script step."))
			paused ? rigs.Resume() : rigs.Pause();
		ImGui::SameLine();
		ImGui::BeginDisabled(!paused);
		if (IconButton("Step script", Icon::StepForward, "Run one script step."))
			rigs.Step();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (IconButton("Stop script", Icon::Stop, "Stop and release its inputs"))
			rigs.Cancel();
	}
	else
	{
		ImGui::BeginDisabled(!runtime.CanReplay() || rigs.IsActive() || rigs.WaitingForLaunch());
		if (IconButton("Run script", Icon::Play, "Run on the selected session", false, "Run"))
		{
			m_error.clear();
			m_report.clear();
			m_run = ScriptRun{path, m_options};
		}
		ImGui::EndDisabled();
	}
	if (mine)
	{
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s, %llu steps", rigs.Error().empty() ? PhaseName(state.phase) : "Failed",
							static_cast<unsigned long long>(state.transactions));
		for (const auto* error : {&rigs.Error(), &state.error})
			if (!error->empty())
				ImGui::TextColored(theme::Vec(theme::Record), "%s", error->c_str());
		for (const auto& checkpoint : state.checkpoints)
		{
			ImGui::TextColored(theme::Vec(checkpoint.passed ? theme::Ok : theme::Record), "%s",
							   checkpoint.passed ? "PASS" : "FAIL");
			ImGui::SameLine();
			ImGui::TextUnformatted(checkpoint.name.c_str());
			if (!checkpoint.detail.empty())
				ImGui::SetItemTooltip("%s", checkpoint.detail.c_str());
		}
		if (!state.output.empty() && Section("Output"))
		{
			ImGui::BeginChild("Script output", {0, 160 * unit}, ImGuiChildFlags_Borders);
			for (const auto& line : state.output)
				ImGui::TextUnformatted(line.c_str());
			ImGui::EndChild();
		}
		ImGui::BeginDisabled(active || state.phase == automation::RigPhase::Idle || rigs.ReportBusy());
		const auto base = Utf8Path(path).replace_extension();
		for (const auto& [label, junit, suffix] : {std::tuple{"Save JSON report", false, ".report.json"},
												   std::tuple{"Save JUnit report", true, ".junit.xml"}})
		{
			if (junit)
				ImGui::SameLine();
			if (ImGui::Button(label))
			{
				const auto target = PathText(base) + suffix;
				m_error = rigs.SaveReport(target, junit);
				m_report.clear();
			}
		}
		ImGui::EndDisabled();
		if (rigs.ReportBusy())
			ImGui::TextDisabled("Saving report…");
		if (!m_report.empty())
		{
			ImGui::TextDisabled("Saved %s", PathText(Utf8Path(m_report).filename()).c_str());
			ImGui::SetItemTooltip("%s", m_report.c_str());
		}
	}
}
} // namespace simxr::app
