#include "app/pages/Library.h"
#include "Pch.h"
#include "app/RuntimePanels.h"
#include "app/Theme.h"
#include "app/services/CompanionSettings.h"
#include "app/services/Utf8Path.h"
#include "app/widgets/Controls.h"

#include <cctype>
#include <cstdio>
#include <imgui.h>
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
void Library::Poll(Selection& selection)
{
	if (m_catalog)
		if (auto result = m_catalog->TakeResult())
		{
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
std::optional<ScriptRun> Library::Draw(Selection& selection, RuntimePanels& runtime, RigPanel& rigs)
{
	Poll(selection);
	if (const auto take = std::get_if<TakeSel>(&selection))
		DrawTake(selection, std::string(take->id), runtime, rigs);
	else if (const auto script = std::get_if<ScriptSel>(&selection))
		DrawScript(selection, std::string(script->id), runtime, rigs);
	return std::exchange(m_run, {});
}
std::optional<ScriptRun> Library::DrawBrowser(Selection& selection, RuntimePanels& runtime, RigPanel& rigs)
{
	Poll(selection);
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
		ImGui::TextDisabled("Showing 512 entries; open other scripts by path.");
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
			if (ImGui::Selectable(entry.name.c_str(), false,
								  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
			{
				if (entry.take)
					selection = TakeSel{entry.path};
				else
					selection = ScriptSel{entry.path};
			}
			ImGui::SetItemTooltip("%s%s%s", entry.path.c_str(), entry.error.empty() ? "" : "\n", entry.error.c_str());
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%s", Age(entry.modified).c_str());
			ImGui::TableNextColumn();
			ImGui::BeginDisabled(!idle || !entry.error.empty());
			if (IconButton("Play entry", Icon::Play,
						   entry.take ? "Restore recorded inputs on the selected app; not game state"
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
bool Library::DrawHeader(Selection& selection, const std::string& path, Icon kind)
{
	const auto name = PathText(Utf8Path(path).stem());
	PageTitle(kind, name.c_str());
	ImGui::SetItemTooltip("%s", path.c_str());
	const float button = ImGui::GetFrameHeight(), spacing = ImGui::GetStyle().ItemSpacing.x;
	ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - button * 3 - spacing * 2));
	ImGui::BeginDisabled(!m_catalog || m_catalog->Busy());
	if (IconButton("Rename entry", Icon::Rename, "Change this file name on disk"))
	{
		std::snprintf(m_rename.data(), m_rename.size(), "%s", name.c_str());
		ImGui::OpenPopup("Rename");
	}
	ImGui::SameLine();
	if (IconButton("Delete entry", Icon::Delete, "Delete this file from disk; asks for confirmation", false, nullptr,
				   theme::Record))
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
		ImGui::BeginDisabled(!valid || !m_catalog || m_catalog->Busy());
		if (ImGui::Button("Rename") || (entered && valid && m_catalog && !m_catalog->Busy()))
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
		ImGui::BeginDisabled(!m_catalog || m_catalog->Busy());
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
void Library::DrawTake(Selection& selection, const std::string& path, RuntimePanels& runtime, RigPanel& rigs)
{
	if (!DrawHeader(selection, path, Icon::Record))
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
		ImGui::BeginDisabled(!runtime.CanReplay() || rigs.IsActive() || rigs.WaitingForLaunch());
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
	if (mine && !rigs.Error().empty())
		ImGui::TextColored(theme::Vec(theme::Record), "%s", rigs.Error().c_str());
}
void Library::DrawScript(Selection& selection, const std::string& path, RuntimePanels& runtime, RigPanel& rigs)
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
		if (paused ? IconButton("Resume script", Icon::Play, "Continue executing this paused script")
				   : IconButton("Pause script", Icon::Pause, "Hold this script before its next transaction"))
			paused ? rigs.Resume() : rigs.Pause();
		ImGui::SameLine();
		ImGui::BeginDisabled(!paused);
		if (IconButton("Step script", Icon::StepForward, "Step one transaction"))
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
		ImGui::TextDisabled("%s · %llu transactions", rigs.Error().empty() ? PhaseName(state.phase) : "Failed",
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
		ImGui::BeginDisabled(active || state.phase == automation::RigPhase::Idle);
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
				m_report = m_error.empty() ? target : std::string{};
			}
		}
		ImGui::EndDisabled();
		if (!m_report.empty())
		{
			ImGui::TextDisabled("Saved %s", PathText(Utf8Path(m_report).filename()).c_str());
			ImGui::SetItemTooltip("%s", m_report.c_str());
		}
	}
}
} // namespace simxr::app
