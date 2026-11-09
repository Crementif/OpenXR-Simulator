#include "app/pages/PhysicalHeadsetPage.h"
#include "app/UiPch.h"
#include "app/services/SimulationEdits.h"
#include "app/services/Utf8Path.h"
#include "app/widgets/Controls.h"
#include "app/widgets/Theme.h"

namespace simxr::app
{
struct PhysicalHeadsetPage::FileResult
{
	std::mutex mutex;
	bool done = false, failed = false;
	std::string path, error;

	static void SDLCALL Picked(void* user, const char* const* files, int) noexcept
	{
		const std::unique_ptr<std::shared_ptr<FileResult>> owner(static_cast<std::shared_ptr<FileResult>*>(user));
		std::lock_guard lock((*owner)->mutex);
		(*owner)->done = true;
		try
		{
			if (!files)
			{
				(*owner)->failed = true;
				(*owner)->error = SDL_GetError();
			}
			else if (files[0])
				(*owner)->path = files[0];
		}
		catch (...)
		{
			(*owner)->failed = true;
		}
	}
};

static std::string DetailValue(std::string_view key, const std::string& value)
{
	if (key == "selection")
		return value == "system" ? "System runtime" : value;
	if (key == "physical_reference_space")
		return value == "1"			   ? "View"
			   : value == "2"		   ? "Local"
			   : value == "3"		   ? "Stage"
			   : value == "1000426000" ? "Local floor"
									   : value;
	if (key == "physical_session_state")
	{
		static constexpr const char* s_states[] = {"Unknown", "Idle",	  "Ready",		  "Synchronized", "Visible",
												   "Focused", "Stopping", "Loss pending", "Exiting"};
		unsigned index = 0;
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), index);
		return parsed.ec == std::errc{} && index < std::size(s_states) ? s_states[index] : value;
	}
	if (key.starts_with("eye ") && value.starts_with("recommended="))
		return value.substr(std::string_view("recommended=").size()) + " recommended";
	return value;
}

// The facts worth reading at a glance; Copy report carries everything else.
static void DrawHostDetails(const HeadsetHostSnapshot& host)
{
	static constexpr std::pair<const char*, const char*> s_shown[] = {{"runtime", "Runtime"},
																	  {"version", "Version"},
																	  {"selection", "Source"},
																	  {"gpu", "GPU"},
																	  {"physical_reference_space", "Tracking space"},
																	  {"physical_session_state", "Session"},
																	  {"eye 0", "Left eye"},
																	  {"eye 1", "Right eye"},
																	  {"body_forwarding", "Body"}};
	if (ImGui::BeginTable("Host details", 2, ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6);
		for (const auto& [key, label] : s_shown)
		{
			const auto found =
				std::ranges::find(host.details, std::string_view(key), &std::pair<std::string, std::string>::first);
			if (found == host.details.end())
				continue;
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%s", label);
			ImGui::TableNextColumn();
			ImGui::TextWrapped("%s", DetailValue(key, found->second).c_str());
		}
		ImGui::EndTable();
	}
	if (!host.extensions.empty())
	{
		const auto label = std::to_string(host.extensions.size()) + " runtime extensions###Extensions";
		if (ImGui::TreeNode(label.c_str()))
		{
			for (const auto& extension : host.extensions)
				ImGui::TextDisabled("%s", extension.c_str());
			ImGui::TreePop();
		}
	}
	if (!host.diagnostics.empty())
	{
		// Only the latest lines; earlier output is in the copied report.
		std::string_view log = host.diagnostics;
		while (log.ends_with('\n'))
			log.remove_suffix(1);
		std::size_t start = log.size();
		for (int lines = 0; lines < 6 && start != std::string_view::npos && start > 0; ++lines)
			start = log.rfind('\n', start - 1);
		log.remove_prefix(start == std::string_view::npos ? 0 : start + 1);
		ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
		ImGui::TextWrapped("%.*s", static_cast<int>(log.size()), log.data());
		ImGui::PopStyleColor();
	}
	if (ImGui::SmallButton("Copy report"))
	{
		std::string report;
		for (const auto& [key, value] : host.details)
			report += key + '=' + value + '\n';
		for (const auto& extension : host.extensions)
			report += "extension=" + extension + '\n';
		report += host.diagnostics;
		ImGui::SetClipboardText(report.c_str());
	}
	ImGui::SetItemTooltip("Copy the headset host's full report, for example for a bug report");
}

void PhysicalHeadsetPage::SetError(std::string error)
{
	m_error = std::move(error);
}

std::optional<HeadsetPageRequest> PhysicalHeadsetPage::Draw(const HeadsetHostSnapshot& host, bool canConnect,
															const std::string& conflict, const ObservedSetup* game)
{
	if (m_file)
	{
		const auto file = m_file;
		std::lock_guard lock(file->mutex);
		if (file->done)
		{
			if (file->failed)
				m_error = file->error.empty() ? "Cannot select a runtime manifest" : file->error;
			else if (!file->path.empty())
				m_manifest = file->path;
			m_file.reset();
		}
	}
	SectionLabel("Physical headset");
	ImGui::PushID("physical-headset");
	const bool active = host.phase == HeadsetHostPhase::Starting || host.phase == HeadsetHostPhase::Running ||
						host.phase == HeadsetHostPhase::Stopping;
	const char* state = "Disconnected";
	ImU32 dot = theme::TextDim;
	switch (host.phase)
	{
	case HeadsetHostPhase::Starting:
		state = "Starting host";
		dot = theme::Warn;
		break;
	case HeadsetHostPhase::Running:
		state = host.input == HeadsetInputState::Ready		 ? "Input connected"
				: host.input == HeadsetInputState::Suspended ? "Input suspended"
															 : "Waiting for input";
		dot = host.input == HeadsetInputState::Ready ? theme::Ok : theme::Warn;
		break;
	case HeadsetHostPhase::Stopping:
		state = "Disconnecting";
		dot = theme::Warn;
		break;
	case HeadsetHostPhase::Failed:
		state = "Connection failed";
		dot = theme::Record;
		break;
	default:
		break;
	}
	if (host.discovering && host.phase == HeadsetHostPhase::Running)
	{
		state = host.recommendedEyes ? "Headset detected" : "Detecting headset";
		dot = host.recommendedEyes ? theme::Ok : theme::Warn;
	}
	const float unit = ImGui::GetFontSize() / 17;
	const auto line = ImGui::GetCursorScreenPos();
	ImGui::GetWindowDrawList()->AddCircleFilled({line.x + 4 * unit, line.y + ImGui::GetTextLineHeight() / 2}, 4 * unit,
												dot);
	ImGui::Indent(20 * unit);
	ImGui::TextUnformatted(state);
	if (host.phase == HeadsetHostPhase::Running && !host.discovering)
	{
		const char* output = host.output == HeadsetOutputState::Live		 ? "Live game output"
							 : host.output == HeadsetOutputState::Inspection ? "Inspection screen"
							 : host.output == HeadsetOutputState::Hidden	 ? "Headset is not rendering"
																			 : "Waiting for game output";
		ImGui::TextDisabled("%s", output);
		ImGui::SetItemTooltip("Paused or stale game output appears on a fixed inspection screen in the headset.");
		if (host.recommendedEyes)
			ImGui::TextDisabled("Headset recommends %u x %u per eye", (*host.recommendedEyes)[0].width,
								(*host.recommendedEyes)[0].height);
	}
	else if (!active)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
		ImGui::TextWrapped("Play through your headset, then inspect or record");
		ImGui::PopStyleColor();
	}
	if (host.trackerSupport && (m_trackers || host.connectedTrackers))
	{
		if (!*host.trackerSupport)
			ImGui::TextDisabled("Trackers unavailable in this runtime");
		else if (host.connectedTrackers)
			ImGui::TextDisabled("%u trackers connected", *host.connectedTrackers);
	}
	ImGui::Unindent(20 * unit);

	// Problems that block or change Connect stay visible while the options are folded.
	const auto warn = [](const char* text) {
		ImGui::PushStyleColor(ImGuiCol_Text, theme::Warn);
		ImGui::TextWrapped("%s", text);
		ImGui::PopStyleColor();
	};
	const bool configurable = !active || host.discovering;
	if (configurable && m_customRuntime && m_manifest.empty())
		warn("Choose a runtime manifest in Options, or use the system runtime.");
	if (m_body && host.bodySupport[m_body <= 2 ? m_body - 1 : 2] == false)
		warn("The selected body provider is unavailable in the detected runtime.");
	if (m_trackers && game && game->appTrackersKnown && !game->appTrackers)
		warn("The game did not enable tracker access. Restart it with tracker support to use trackers.");
	if (configurable && !conflict.empty())
	{
		const std::string takeover = conflict +
									 (m_trackers ? " Connecting takes over head, view, hand and tracker"
												 : " Connecting takes over head, view and hand") +
									 (m_body ? " input, and Body." : " input.");
		warn(takeover.c_str());
	}

	std::optional<HeadsetPageRequest> request;
	// Action buttons share a row while they fit.
	const auto beside = [](const char* label) {
		ImGui::SameLine();
		if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2)
			ImGui::NewLine();
	};
	const char* stop = host.discovering ? "Stop detection" : "Disconnect";
	const bool blocked = static_cast<bool>(m_file) || (m_customRuntime && m_manifest.empty());
	if (configurable)
	{
		ImGui::BeginDisabled(!canConnect || blocked || (active && host.phase != HeadsetHostPhase::Running));
		ImGui::PushStyleColor(ImGuiCol_Button, theme::Accent);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::Mix(theme::Accent, theme::Text, .15f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::Mix(theme::Accent, theme::Text, .25f));
		if (ImGui::Button(conflict.empty() ? "Connect" : "Take over and connect"))
		{
			m_error.clear();
			request = HeadsetPageRequest{true};
			request->options.takeOver = !conflict.empty();
			request->options.trackers = m_trackers;
			if (m_body)
				request->options.body = static_cast<BodyJointSet>(m_body - 1);
			if (m_customRuntime)
				request->options.runtimeManifest = Utf8Path(m_manifest);
			if (m_calibrateLocal)
				request->options.localEyeHeight = m_eyeHeight;
		}
		ImGui::PopStyleColor(3);
		ImGui::SetItemTooltip(canConnect ? "Play the running app through your headset, aligned with the current player"
										 : "Needs a running app session");
		ImGui::EndDisabled();
		beside(active ? stop : "Detect headset");
	}
	if (active)
	{
		ImGui::BeginDisabled(host.phase == HeadsetHostPhase::Stopping);
		if (ImGui::Button(stop))
			request = HeadsetPageRequest{};
		ImGui::EndDisabled();
		if (!host.discovering)
		{
			beside("Re-align");
			ImGui::BeginDisabled(host.phase != HeadsetHostPhase::Running || host.input != HeadsetInputState::Ready ||
								 !canConnect);
			if (ImGui::Button("Re-align"))
			{
				m_error.clear();
				request = HeadsetPageRequest{false, true};
			}
			ImGui::SetItemTooltip("Reconnect the headset at the current simulated pose. The game stays running; input "
								  "pauses briefly.");
			ImGui::EndDisabled();
		}
	}
	else
	{
		ImGui::BeginDisabled(blocked);
		if (ImGui::Button("Detect headset"))
		{
			m_error.clear();
			request = HeadsetPageRequest{true};
			request->options.discover = true;
			if (m_customRuntime)
				request->options.runtimeManifest = Utf8Path(m_manifest);
		}
		ImGui::SetItemTooltip("Find render-size recommendations before launching a game. "
							  "Use headset size in Launch profiles saves them for the next launch.");
		ImGui::EndDisabled();
	}
	for (const std::string* error : {&std::as_const(m_error), &host.error})
		if (!error->empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, theme::Record);
			ImGui::TextWrapped("%s", error->c_str());
			ImGui::PopStyleColor();
		}

	static constexpr const char* s_bodies[] = {"Off", "FB upper body", "META full body", "BD without arms",
											   "BD full body"};
	const bool options = Section("Options");
	if (!options)
	{
		std::string summary = m_customRuntime ? "Custom runtime" : "System runtime";
		if (m_trackers)
			summary += ", trackers";
		if (m_body)
			summary += std::string(", ") + s_bodies[m_body];
		if (m_calibrateLocal)
			summary += ", LOCAL height";
		ImGui::SameLine();
		ImGui::PushFont(nullptr, ImGui::GetFontSize() * .85f);
		ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
		ImGui::TextWrapped("%s", summary.c_str());
		ImGui::PopStyleColor();
		ImGui::PopFont();
	}
	else
	{
		ImGui::BeginDisabled(active || static_cast<bool>(m_file));
		ImGui::Checkbox("Custom runtime", &m_customRuntime);
		if (m_customRuntime)
		{
			if (ImGui::Button("Choose runtime manifest"))
			{
				m_file = std::make_shared<FileResult>();
				auto owner = std::make_unique<std::shared_ptr<FileResult>>(m_file);
				SDL_ShowOpenFileDialog(FileResult::Picked, owner.release(), nullptr, nullptr, 0,
									   m_manifest.empty() ? nullptr : m_manifest.c_str(), false);
			}
			ImGui::TextWrapped("%s", m_manifest.empty() ? "No manifest selected" : m_manifest.c_str());
		}
		ImGui::Checkbox("Forward trackers", &m_trackers);
		ImGui::SetItemTooltip(
			"Use physical HTCX trackers. Requires runtime support and takes ownership of the tracker inventory.");
		ImGui::SetNextItemWidth(-1);
		ImGui::Combo("##body-provider", &m_body,
					 "Body: Off\0FB upper body\0META full body\0BD without arms\0BD full body\0");
		ImGui::SetItemTooltip(
			"Forward runtime-estimated body joints. Takes ownership of Body; requires the selected runtime extension.");
		if (m_body && game)
		{
			if (!game->appBodyExtensions)
				ImGui::TextWrapped("Game body access is unknown with this simulator version.");
			else
			{
				const auto& enabled = *game->appBodyExtensions;
				ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
				ImGui::TextWrapped("Game body access: FB %s, META full %s, BD %s", enabled[0] ? "on" : "off",
								   enabled[1] ? "on" : "off", enabled[2] ? "on" : "off");
				ImGui::PopStyleColor();
				if (!enabled[0] && !enabled[1] && !enabled[2])
					ImGui::TextWrapped("Restart the game with body tracking support to use forwarded joints. Capture "
									   "remains available.");
			}
		}
		ImGui::Checkbox("Calibrate LOCAL height", &m_calibrateLocal);
		ImGui::SetItemTooltip("Only needed when the physical runtime supplies no floor-based tracking space.");
		if (m_calibrateLocal)
		{
			ImGui::SetNextItemWidth(-1);
			ImGui::InputFloat("##eye-height", &m_eyeHeight, 0, 0, "%.3f m");
			ImGui::SetItemTooltip("Measured eye height above the floor. Used only with LOCAL tracking.");
		}
		ImGui::EndDisabled();
	}
	if ((!host.details.empty() || !host.extensions.empty() || !host.diagnostics.empty()) &&
		Section("Connection details"))
		DrawHostDetails(host);
	ImGui::PopID();
	return request;
}
} // namespace simxr::app
