#include "app/UiPch.h"
#include "Cases.h"
#include "app/dialogs/LaunchProfilesDialog.h"
#include "app/widgets/RecordingPanel.h"
#include "app/widgets/AppPicker.h"
#include "app/widgets/Notices.h"
#include "app/widgets/TimelineCanvas.h"
#include "app/dialogs/PreferencesDialog.h"
#include "app/services/HistoryStore.h"
#include "app/services/InputHistory.h"
#include "app/widgets/Theme.h"
#include "app/services/Utf8Path.h"
#include <iostream>
#include <random>

namespace simxr::tests
{
void CheckNotices(Interaction& test)
{
	Notices notices;
	unsigned opened = 0, replaced = 0;
	notices.Push({"A saved take is ready", "A long description stays in the tooltip and notice history", "Play", [&] {
					  ++opened;
					  notices.Push({"Opened", "The action replaced its own notice collection"});
				  }});
	test.extraDraw = [&] {
		notices.Draw({70, 80, 300, 240});
	};
	test.Frame();
	test.Frame();
	auto* window = FindPanel("##Notices");
	Require(window && window->Pos.x >= 70 && window->Pos.x + window->Size.x <= 370,
			"Notice escaped the preview's width");
	if (const auto directory = std::getenv("SIMXR_DIALOG_QA_DIR"))
	{
		test.renderer.RequestScreenshot(std::filesystem::path(directory) / "bounded-notice.bmp");
		test.Frame();
	}
	const auto activate = [&](unsigned id, const char* label) {
		ImGui::ActivateItemByID(ImHashStr(label, 0, window->GetID(std::to_string(id).c_str())));
		test.Frame();
	};
	activate(1, "Play");
	Require(opened == 1 && notices.Latest() && notices.Latest()->title == "Opened",
			"Notice action could not safely replace its own entries");
	notices.Push({"Unavailable action", "", "Open", [] { throw std::runtime_error("No longer available"); }});
	test.Frame();
	activate(3, "Open");
	Require(notices.Latest() && notices.Latest()->title == "Action failed" &&
				notices.Latest()->detail == "No longer available",
			"Notice action failure escaped or lost its explanation");
	notices.Push({"Replacement", "", "Open", [&] { ++opened; }});
	test.Frame();
	const auto old = ImHashStr("Open", 0, window->GetID("5"));
	notices.Push({"Replacement", "", "Open", [&] { ++replaced; }});
	ImGui::ActivateItemByID(old);
	test.Frame();
	Require(opened == 1 && replaced == 0, "A replacement notice inherited the retired action's activation");
	activate(6, "Open");
	Require(replaced == 1, "The replacement notice's own action was unavailable");
	test.extraDraw = {};
	test.Frame();
	std::cout << "PASS bounded notices, reentrant actions, failure handling and replacement identity\n";
}

void CheckTimelineCanvas(Interaction& test)
{
	HistoryStore history;
	for (std::uint64_t frame = 1; frame <= 54000; ++frame)
		history.Append(
			{{1, 1, frame}, 1, 0, double(frame) / 90, 11.11f, (frame == 200 ? HistoryDropped : 0U) | HistoryMarker});
	history.SetFlags(49999, HistorySnapshot, true);
	HistoryStore emptyHistory;
	HistoryStore replacement{2};
	replacement.Append({{1, 1, 27000}, 2, 0, 300, 11.11f});
	replacement.Append({{1, 1, 50000}, 2, 0, 555, 11.11f});
	bool populated = true, visible = true, overlap = false, replaced = false, following = false;
	double canvasSeconds = 0;
	TimelineCanvas canvas;
	canvas.Fit();
	TimelineGesture selected, lastGesture;
	ImVec2 point, clipPoint, markerPoint, snapshotPoint, outsidePoint;
	int plays = 0, otherPlays = 0, pins = 0, selections = 0;
	test.extraDraw = [&] {
		if (!visible)
			return;
		ImGui::SetNextWindowPos({30, 170}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({740, 240}, ImGuiCond_Always);
		ImGui::Begin("Timeline interaction");
		const auto top = ImGui::GetCursorScreenPos();
		const auto available = ImGui::GetContentRegionAvail();
		const float labels = std::min(available.x * .25f, ImGui::GetFontSize() * 6);
		point = {top.x + labels + (available.x - labels) * .5f, top.y + 10};
		const auto drawStarted = std::chrono::steady_clock::now();
		canvas.Begin(labels, 0);
		const auto& shownHistory = !populated ? emptyHistory : replaced ? replacement : history;
		const auto gesture = canvas.Draw(shownHistory, test.runtime.KeptPreviews(), {0, 0, 1, 1}, following);
		if (gesture.stopFollowing)
			following = false;
		const auto bounds = canvas.Bounds();
		const auto range = canvas.VisibleRange();
		clipPoint = {bounds.x + float((200 - range.first) / (range.second - range.first)) * bounds.width + 5,
					 bounds.y + canvas.TransportRect().height + 5};
		const float scale = ImGui::GetFontSize() / 16;
		markerPoint = {bounds.x + bounds.width * .8f, bounds.y + canvas.TransportRect().height - 5 * scale};
		snapshotPoint = {
			bounds.x + std::floor(float((history.At(49999).seconds - range.first) / (range.second - range.first)) *
								  bounds.width),
			bounds.y + canvas.TransportRect().height + (bounds.height - canvas.TransportRect().height) / 2};
		outsidePoint = {bounds.x - 12, point.y};
		if (canvas.DrawClip("Saved take", 200, 250, "Saved take", false, true) == ClipHit::Play)
			++plays;
		if (overlap && canvas.DrawClip("Another take", 200, 250, "Another take", false, true) == ClipHit::Play)
			++otherPlays;
		canvasSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - drawStarted).count();
		lastGesture = gesture;
		if (gesture.first)
			++selections;
		if (gesture.pin)
			++pins;
		if (gesture.first || gesture.marker || gesture.pin)
			selected = gesture;
		ImGui::End();
	};
	for (int i = 0; i < 3; ++i)
		test.Frame();
	test.Click(point);
	Require(selected.first && !selected.last && *selected.first > 26000 && *selected.first < 28000,
			"Canvas click did not hit-test ten-minute history");
	test.Click(clipPoint);
	Require(plays == 1, "Clip play click did not activate");
	SDL_Event drag{};
	drag.type = SDL_EVENT_MOUSE_MOTION;
	drag.motion.windowID = SDL_GetWindowID(test.window.Native());
	drag.motion.x = point.x;
	drag.motion.y = point.y;
	SDL_PushEvent(&drag);
	test.Frame();
	drag = {};
	drag.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
	drag.button.windowID = SDL_GetWindowID(test.window.Native());
	drag.button.button = SDL_BUTTON_LEFT;
	drag.button.down = true;
	SDL_PushEvent(&drag);
	test.Frame();
	drag = {};
	drag.type = SDL_EVENT_MOUSE_MOTION;
	drag.motion.windowID = SDL_GetWindowID(test.window.Native());
	drag.motion.x = clipPoint.x;
	drag.motion.y = clipPoint.y;
	SDL_PushEvent(&drag);
	test.Frame();
	drag = {};
	drag.type = SDL_EVENT_MOUSE_BUTTON_UP;
	drag.button.windowID = SDL_GetWindowID(test.window.Native());
	drag.button.button = SDL_BUTTON_LEFT;
	SDL_PushEvent(&drag);
	test.Frame();
	Require(plays == 1, "Range drag release activated clip playback");
	SDL_Event key{};
	key.type = SDL_EVENT_KEY_DOWN;
	key.key.windowID = SDL_GetWindowID(test.window.Native());
	key.key.key = SDLK_M;
	key.key.scancode = SDL_SCANCODE_M;
	key.key.down = true;
	SDL_PushEvent(&key);
	test.Frame();
	key.type = SDL_EVENT_KEY_UP;
	key.key.down = false;
	SDL_PushEvent(&key);
	test.Frame();
	Require(selected.marker.has_value(), "M did not mark the hovered timeline frame");

	const auto move = [&](ImVec2 position) {
		ImGui::GetIO().AddMousePosEvent(position.x, position.y);
		test.Frame();
	};
	const auto button = [&](bool down) {
		ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		test.Frame();
	};
	following = true;
	test.Frame();
	move(point);
	button(true);
	Require(lastGesture.stopFollowing && !following, "Canvas press did not stop the moving live axis");
	move(markerPoint);
	button(false);
	Require(lastGesture.first && lastGesture.last, "Marker release collapsed a ruler range to one frame");

	const auto beforeOutside = selections;
	move(outsidePoint);
	button(true);
	move(snapshotPoint);
	button(false);
	Require(pins == 0 && selections == beforeOutside,
			"Releasing an outside press over a snapshot pinned or selected it");
	test.Click(snapshotPoint);
	Require(pins == 1, "Owned snapshot click did not pin its frame");

	const auto beforeHidden = selections;
	move(point);
	button(true);
	visible = false;
	test.Frame();
	test.Frame();
	visible = true;
	move(markerPoint);
	button(false);
	Require(selections == beforeHidden, "A hidden canvas kept an old range gesture alive");

	move(point);
	button(true);
	populated = false;
	test.Frame();
	populated = true;
	move(markerPoint);
	button(false);
	Require(selections == beforeHidden, "Clearing history retained an old frame press");

	move(point);
	button(true);
	replaced = true;
	move(markerPoint);
	button(false);
	Require(selections == beforeHidden, "A frame press selected replacement history from another epoch");
	replaced = false;
	test.Frame();

	overlap = true;
	test.Click(clipPoint);
	Require(plays == 1 && otherPlays == 1, "Overlapping clips with the same start shared a play press");
	overlap = false;
	populated = false;
	const auto baselineStarted = std::chrono::steady_clock::now();
	for (int i = 0; i < 60; ++i)
		test.Frame();
	const double baseline = std::chrono::duration<double>(std::chrono::steady_clock::now() - baselineStarted).count();
	populated = true;
	canvasSeconds = 0;
	const auto started = std::chrono::steady_clock::now();
	for (int i = 0; i < 120; ++i)
	{
		canvas.Zoom(i * 4, i * 4 + 120);
		test.Frame();
	}
	const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	std::cout << "PASS native canvas hit-testing and marker; 54000 records, " << 120 / elapsed << " UI frames/s; empty "
			  << 60 / baseline << "; canvas CPU " << canvasSeconds * 1000 / 120 << " ms/frame\n";

	HistoryStore ghosts{16};
	const auto appendGhost = [&](std::uint64_t run, std::uint64_t frame, double seconds, std::uint32_t flags,
								 std::uint8_t hand) {
		HistoryFrame sample{{41, run, frame}, 3, 0, seconds, 11.11f, flags};
		sample.hands[0] = hand;
		ghosts.Append(sample);
	};
	appendGhost(7, 10, 0, 0, 64);
	appendGhost(7, 12, 1, HistoryGap, 96);
	appendGhost(7, 13, 2, 0, 128);
	appendGhost(7, 14, 3, 0, 160);
	appendGhost(7, 15, 4, 0, 192);
	appendGhost(8, 16, 5, HistoryRunStart, 255);
	appendGhost(9, 100, 10, HistoryDriven, 0);
	appendGhost(9, 101, 11, HistoryDriven, 0);
	appendGhost(9, 103, 13, HistoryDriven | HistoryGap, 0);
	appendGhost(9, 104, 14, 0, 0);
	appendGhost(9, 105, 15, HistoryDriven, 0);
	appendGhost(10, 106, 16, HistoryDriven | HistoryRunStart, 0);
	appendGhost(9, 100, 17, HistoryDriven | HistoryRunStart, 0);
	HistoryStore denseGhosts{6};
	for (std::uint64_t frame = 0; frame < 6; ++frame)
	{
		HistoryFrame sample{
			{1, frame < 3 ? 1U : 2U, frame < 3 ? frame + 1 : frame + 97},  1,	   0,
			frame < 3 ? double(frame) : 10 + double(frame - 3) * .0000001, 11.11f, frame < 3 ? 0U : HistoryDriven};
		sample.hands[0] = frame == 1 ? 255 : 64;
		denseGhosts.Append(sample);
	}
	TimelineCanvas ghostCanvas;
	ghostCanvas.Zoom(9, 18);
	std::vector<ImVec2> boundedGhost, scopedGhost, sourceBoundGhost, denseDrivenGhost, denseReleasedGhost;
	ShellRect ghostBounds;
	std::pair<double, double> ghostRange;
	float ghostScale = 1;
	test.extraDraw = [&] {
		ImGui::SetNextWindowPos({30, 170}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({740, 240}, ImGuiCond_Always);
		ImGui::Begin("Timeline ghost interaction");
		ghostCanvas.Begin(0, 0);
		ghostCanvas.Draw(ghosts, test.runtime.KeptPreviews(), {0, 0, 1, 1}, false);
		ghostBounds = ghostCanvas.Bounds();
		ghostRange = ghostCanvas.VisibleRange();
		ghostScale = ImGui::GetFontSize() / 16;
		auto* draw = ImGui::GetWindowDrawList();
		const auto capture = [&](const HistoryStore& samples, std::size_t sourceLast, std::size_t targetFirst,
								 std::size_t targetLast) {
			const int first = draw->VtxBuffer.Size;
			ghostCanvas.DrawGhost(samples, 0, sourceLast, targetFirst, targetLast);
			std::vector<ImVec2> points;
			for (int vertex = first; vertex < draw->VtxBuffer.Size; ++vertex)
				points.push_back(draw->VtxBuffer[vertex].pos);
			return points;
		};
		boundedGhost = capture(ghosts, 4, 6, 8);
		scopedGhost = capture(ghosts, 5, 6, 12);
		sourceBoundGhost = capture(ghosts, 2, 6, 12);
		denseDrivenGhost = capture(denseGhosts, 2, 3, 5);
		denseGhosts.SetFlags(4, HistoryDriven, false);
		denseReleasedGhost = capture(denseGhosts, 2, 3, 5);
		denseGhosts.SetFlags(4, HistoryDriven, true);
		ImGui::End();
	};
	test.Frame();
	const auto painted = [&](const std::vector<ImVec2>& points, double seconds) {
		const float x = ghostBounds.x + float((seconds - ghostRange.first) / (ghostRange.second - ghostRange.first)) *
											ghostBounds.width;
		return std::any_of(points.begin(), points.end(),
						   [&](ImVec2 point) { return std::abs(point.x - x) < 4 * ghostScale; });
	};
	Require(painted(boundedGhost, 10) && painted(boundedGhost, 13) && !painted(boundedGhost, 15),
			"Replay ghost ignored its inclusive completion endpoint");
	Require(!painted(scopedGhost, 11) && !painted(scopedGhost, 12),
			"Replay ghost compressed missing source or target frames");
	Require(painted(scopedGhost, 10) && painted(scopedGhost, 13) && painted(scopedGhost, 15) &&
				!painted(scopedGhost, 14) && !painted(scopedGhost, 16) && !painted(scopedGhost, 17),
			"Replay ghost crossed released input or the original run segment");
	Require(painted(sourceBoundGhost, 13) && !painted(sourceBoundGhost, 15),
			"Replay ghost exceeded the take's inclusive source endpoint");
	Require(!denseDrivenGhost.empty() && denseReleasedGhost.empty(),
			"A dense replay ghost hid driven input or included a released frame");
	std::cout << "PASS replay ghost frame gaps, completion bounds and input release\n";
	test.extraDraw = {};
	test.Frame();
}

void CheckPreferences(Interaction& test)
{
	PreferencesDialog dialog;
	CompanionSettings settings;
	McpUiState mcp;
	UpdateUiState update;
	mcp.available = true;
	mcp.configuration = R"({"mcpServers":{"openxr-simulator":{"command":"/tmp/simulator connector"}}})";
	mcp.codexConfiguration = "[mcp_servers.openxr-simulator]\ncommand = \"/tmp/simulator connector\"\nargs = []";
	bool open = true, dirty = false, visible = true;
	unsigned closed = 0;
	ImVec2 closeMinimum, closeMaximum;
	test.workspace.SetScale(1);
	test.extraDraw = [&] {
		ImGui::Begin("Preferences fixture");
		if (visible)
		{
			closed += dialog.Draw(settings, open, dirty, 64ULL << 20, 512ULL << 20, 32ULL << 20, 1024ULL << 20, {}, 0,
								  0, &mcp, &update);
			closeMinimum = ImGui::GetItemRectMin();
			closeMaximum = ImGui::GetItemRectMax();
		}
		ImGui::End();
	};
	const auto pump = [&] {
		test.Frame();
		test.Frame();
	};
	auto& io = ImGui::GetIO();
	const auto tap = [&](ImGuiKey key) {
		io.AddKeyEvent(key, true);
		test.Frame();
		io.AddKeyEvent(key, false);
		test.Frame();
	};
	pump();
	const int action = static_cast<int>(PilotAction::Forward);
	const auto contentsWindow = []() -> ImGuiWindow* {
		for (auto* window : ImGui::GetCurrentContext()->Windows)
			if (window->Active && window->ParentWindow == FindPanel("Workspace settings") &&
				std::string_view(window->Name).find("/Preference contents_") != std::string_view::npos)
				return window;
		return nullptr;
	};
	if (std::getenv("SIMXR_BUDGET_QA"))
	{
		auto* contents = contentsWindow();
		Require(contents, "History settings contents missing");
		const auto general = ImHashStr("General", 0, contents->GetID("Preference pages"));
		ImGui::ActivateItemByID(ImHashStr("History GPU (MiB)", 0, general));
		pump();
		io.AddKeyEvent(ImGuiMod_Ctrl, true);
		tap(ImGuiKey_A);
		io.AddKeyEvent(ImGuiMod_Ctrl, false);
		io.AddInputCharactersUTF8("2048");
		pump();
		tap(ImGuiKey_Enter);
		Require(settings.previewBudgetMiB == 2048 && dirty, "History budget field did not accept typed value");
		ImGui::ActivateItemByID(ImHashStr("History RAM (MiB)", 0, general));
		pump();
		io.AddKeyEvent(ImGuiMod_Ctrl, true);
		tap(ImGuiKey_A);
		io.AddKeyEvent(ImGuiMod_Ctrl, false);
		io.AddInputCharactersUTF8("4096");
		pump();
		tap(ImGuiKey_Enter);
		Require(settings.previewRamMiB == 4096, "RAM history budget field did not accept typed value");
		if (const auto directory = std::getenv("SIMXR_DIALOG_QA_DIR"))
		{
			test.renderer.RequestScreenshot(std::filesystem::path(directory) / "history-budget.bmp");
			test.Frame();
		}
		test.renderer.WaitIdle();
		Require(test.validation->errors.load() == 0, "History budget UI validation errors");
		std::cout << "PASS typed history budget and rendered preferences\n";
		return;
	}
	const auto capture = [&](const char* step) {
		auto* contents = contentsWindow();
		if (!contents || !contents->Active)
		{
			const auto* context = ImGui::GetCurrentContext();
			const auto* modal = FindPanel("Workspace settings");
			std::cout << "Preferences content missing before " << step << ": open=" << open << " visible=" << visible
					  << " closed=" << closed << " focusLost=" << io.AppFocusLost << " active=" << context->ActiveId
					  << " nav=" << context->NavId << " modalActive=" << (modal && modal->Active)
					  << " popups=" << context->OpenPopupStack.Size << '\n';
			const auto directory = std::getenv("SIMXR_DIALOG_QA_DIR");
			const auto image = (directory ? std::filesystem::path(directory) : std::filesystem::temp_directory_path()) /
							   "preferences-content-failure.bmp";
			test.renderer.RequestScreenshot(image);
			test.Frame();
			test.renderer.WaitIdle();
			Require(false, (std::string("Preferences content missing before ") + step).c_str());
		}
		const auto keyboard = ImHashStr("Keys & mouse", 0, contents->GetID("Preference pages"));
		const auto row = ImHashData(&action, sizeof(action), ImHashStr("Bindings", 0, keyboard));
		ImGui::ActivateItemByID(ImHashStr("###Binding", 0, row));
		pump();
	};
	const auto tab = [&](const char* name) {
		auto* contents = contentsWindow();
		Require(contents, "Preferences content missing for tab selection");
		ImGui::ActivateItemByID(ImHashStr(name, 0, contents->GetID("Preference pages")));
		pump();
	};
	if (std::getenv("SIMXR_UPDATE_UI_QA"))
	{
		tab("About");
		auto* contents = contentsWindow();
		Require(contents, "Update preferences contents missing");
		const auto about = ImHashStr("About", 0, contents->GetID("Preference pages"));
		ImGui::ActivateItemByID(ImHashStr("Check for updates at startup", 0, about));
		pump();
		Require(settings.autoUpdateCheck && dirty, "Automatic update preference did not change");
		ImGui::ActivateItemByID(ImHashStr("Check for updates", 0, about));
		pump();
		Require(update.checkRequested, "Manual update check was not requested");
		update.result = {UpdateStatus::Unavailable, {}, {}, "Network unavailable"};
		pump();
		if (const auto directory = std::getenv("SIMXR_DIALOG_QA_DIR"))
		{
			test.renderer.RequestScreenshot(std::filesystem::path(directory) / "update-offline.bmp");
			test.Frame();
		}
		const auto resize = [&](int width, int height) {
			const auto rebuilds = test.renderer.RebuildCount();
			test.window.Resize(width, height);
			Eventually(
				[&] {
					test.Frame();
					return test.renderer.RebuildCount() > rebuilds;
				},
				"Update preferences resize did not rebuild the swapchain");
		};
		resize(760, 600);
		pump();
		if (const auto directory = std::getenv("SIMXR_DIALOG_QA_DIR"))
		{
			test.renderer.RequestScreenshot(std::filesystem::path(directory) / "update-offline-narrow.bmp");
			test.Frame();
		}
		resize(1280, 800);
		update.result = {UpdateStatus::Available, "v2.1.0", "https://github.com/owner/repo/releases/tag/v2.1.0"};
		pump();
		settings.scale = 2.5f;
		test.workspace.SetScale(settings.scale);
		pump();
		contents = contentsWindow();
		Require(contents, "Enlarged update preferences contents missing");
		if (contents->ScrollMax.y > 0)
		{
			ImGui::SetScrollY(contents, contents->ScrollMax.y);
			pump();
		}
		const auto* viewport = ImGui::GetMainViewport();
		Require(closeMinimum.y >= viewport->WorkPos.y && closeMaximum.y <= viewport->WorkPos.y + viewport->WorkSize.y,
				"Enlarged update preferences put Close outside the viewport");
		if (const auto directory = std::getenv("SIMXR_DIALOG_QA_DIR"))
		{
			test.renderer.RequestScreenshot(std::filesystem::path(directory) / "update-available-enlarged.bmp");
			test.Frame();
		}
		resize(760, 600);
		pump();
		if (const auto directory = std::getenv("SIMXR_DIALOG_QA_DIR"))
		{
			test.renderer.RequestScreenshot(std::filesystem::path(directory) / "update-available-narrow-enlarged.bmp");
			test.Frame();
		}
		test.renderer.WaitIdle();
		Require(test.validation->errors.load() == 0, "Update preferences generated Vulkan validation errors");
		test.extraDraw = {};
		test.workspace.SetScale(1);
		std::cout << "PASS update preferences, manual check and availability states\n";
		return;
	}
	if (std::getenv("SIMXR_MCP_UI_QA"))
	{
		tab("MCP");
		auto* contents = contentsWindow();
		const auto mcpTab = ImHashStr("MCP", 0, contents->GetID("Preference pages"));
		const auto activate = [&](const char* name) {
			ImGui::ActivateItemByID(ImHashStr(name, 0, mcpTab));
			pump();
		};
		activate("Enable MCP on startup");
		Require(settings.mcpEnabled && dirty, "MCP startup preference did not change");
		mcp.listening = true;
		mcp.discoverable = true;
		mcp.sessions = 1;
		mcp.httpConfiguration = R"({"url":"http://127.0.0.1:1234/mcp"})";
		pump();
		ImGui::ActivateItemByID(ImHashStr("Copy MCP configuration", 0, mcpTab));
		test.Frame();
		char* clipboard = SDL_GetClipboardText();
		if (!clipboard || mcp.configuration != clipboard)
		{
			std::cout << "MCP clipboard mismatch: " << (clipboard ? clipboard : "<unavailable>") << '\n';
			if (const auto directory = std::getenv("SIMXR_DIALOG_QA_DIR"))
			{
				test.renderer.RequestScreenshot(std::filesystem::path(directory) / "mcp-preferences-failure.bmp");
				test.Frame();
				test.renderer.WaitIdle();
			}
		}
		Require(clipboard && mcp.configuration == clipboard, "MCP configuration was not copied exactly");
		SDL_free(clipboard);
		activate("Client");
		const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
		Require(!popups.empty() && popups.back().Window, "MCP client selector did not open");
		ImGui::ActivateItemByID(ImHashStr("Codex", 0, popups.back().Window->GetID(1)));
		pump();
		activate("Copy MCP configuration");
		clipboard = SDL_GetClipboardText();
		std::string codexClipboard = clipboard ? clipboard : "";
		// The native Windows clipboard adds CR to text line endings.
		for (auto offset = codexClipboard.find("\r\n"); offset != std::string::npos;
			 offset = codexClipboard.find("\r\n", offset))
			codexClipboard.erase(offset, 1);
		Require(clipboard && mcp.codexConfiguration == codexClipboard, "Codex configuration was not copied correctly");
		SDL_free(clipboard);
		if (const auto directory = std::getenv("SIMXR_DIALOG_QA_DIR"))
		{
			test.renderer.RequestScreenshot(std::filesystem::path(directory) / "mcp-preferences-normal.bmp");
			test.Frame();
		}
		activate("Stop MCP");
		Require(!settings.mcpEnabled && mcp.stopRequested, "Stop MCP did not clear the preference and request stop");
		mcp.listening = false;
		mcp.error = "The local endpoint is unavailable.";
		pump();
		activate("Retry MCP");
		Require(settings.mcpEnabled && mcp.retryRequested, "Retry MCP did not enable and request retry");
		settings.scale = 2.5f;
		test.workspace.SetScale(settings.scale);
		pump();
		const auto* viewport = ImGui::GetMainViewport();
		Require(closeMinimum.y >= viewport->WorkPos.y && closeMaximum.y <= viewport->WorkPos.y + viewport->WorkSize.y,
				"Enlarged MCP preferences put Close outside the viewport");
		if (const auto directory = std::getenv("SIMXR_DIALOG_QA_DIR"))
		{
			test.renderer.RequestScreenshot(std::filesystem::path(directory) / "mcp-preferences-enlarged.bmp");
			test.Frame();
		}
		test.renderer.WaitIdle();
		Require(test.validation->errors.load() == 0, "MCP preferences generated Vulkan validation errors");
		test.extraDraw = {};
		test.workspace.SetScale(1);
		std::cout << "PASS MCP preferences, clipboard, stop and retry controls\n";
		return;
	}
	tab("Keys & mouse");
	capture("Ctrl+K chord");
	io.AddKeyEvent(ImGuiMod_Ctrl, true);
	io.AddKeyEvent(ImGuiKey_LeftCtrl, true);
	test.Frame();
	Require(settings.pilotBindings[action] == DefaultPilotBindings()[action] && !dirty,
			"Modifier press finished a binding before its chord key");
	tap(ImGuiKey_K);
	io.AddKeyEvent(ImGuiKey_LeftCtrl, false);
	io.AddKeyEvent(ImGuiMod_Ctrl, false);
	test.Frame();
	const bool chordCaptured = settings.pilotBindings[action] == PilotBinding{"K", 1} && dirty;
	if (!chordCaptured)
	{
		const auto* context = ImGui::GetCurrentContext();
		std::cout << "Preferences capture failure: open=" << open << " focusLost=" << io.AppFocusLost
				  << " active=" << context->ActiveId << " nav=" << context->NavId << " mouse=" << io.MousePos.x << ','
				  << io.MousePos.y << " down=" << io.MouseDown[0]
				  << " binding=" << PilotBindingLabel(settings.pilotBindings[action]) << "\n";
		const auto directory = std::getenv("SIMXR_DIALOG_QA_DIR");
		const auto image = (directory ? std::filesystem::path(directory) : std::filesystem::temp_directory_path()) /
						   "preferences-failure.bmp";
		test.renderer.RequestScreenshot(image);
		test.Frame();
		test.renderer.WaitIdle();
	}
	Require(chordCaptured, "Preferences did not capture Ctrl+K");
	capture("modifier-only binding");
	io.AddKeyEvent(ImGuiMod_Shift, true);
	io.AddKeyEvent(ImGuiKey_RightShift, true);
	test.Frame();
	io.AddKeyEvent(ImGuiKey_RightShift, false);
	io.AddKeyEvent(ImGuiMod_Shift, false);
	test.Frame();
	Require(settings.pilotBindings[action] == PilotBinding{"RightShift", 0},
			"A modifier-only binding did not finish on release");
	const auto bindings = settings.pilotBindings;
	dirty = false;
	capture("tab cancellation");
	tab("Gamepad");
	Require(FindPanel("Gamepad mapping") && FindPanel("Gamepad mapping")->Active, "Gamepad tab did not open");
	tap(ImGuiKey_J);
	tab("Keys & mouse");
	tap(ImGuiKey_L);
	Require(settings.pilotBindings == bindings && !dirty, "Changing tabs left a keyboard binding armed");
	capture("focus cancellation");
	io.AddFocusEvent(false);
	test.Frame();
	io.AddFocusEvent(true);
	test.Frame();
	tap(ImGuiKey_N);
	Require(settings.pilotBindings == bindings && !dirty, "Focus loss left a keyboard binding armed");
	capture("hidden-dialog cancellation");
	visible = false;
	test.Frame();
	visible = true;
	pump();
	tap(ImGuiKey_J);
	Require(settings.pilotBindings == bindings && !dirty, "A hidden Preferences dialog kept its key capture");
	capture("Escape cancellation");
	tap(ImGuiKey_Escape);
	Require(open && !closed, "Esc closed Preferences instead of cancelling key capture");
	tap(ImGuiKey_J);
	Require(settings.pilotBindings == bindings && !dirty, "Esc did not cancel the pending binding");
	if (const auto directory = std::getenv("SIMXR_DIALOG_QA_DIR"))
	{
		test.renderer.RequestScreenshot(std::filesystem::path(directory) / "preferences-normal.bmp");
		test.Frame();
	}
	tap(ImGuiKey_Escape);
	Require(!open && closed == 1, "Esc did not close an idle Preferences dialog");
	open = true;
	settings.scale = 2.5f;
	test.workspace.SetScale(settings.scale);
	pump();
	tab("Gamepad");
	const auto* viewport = ImGui::GetMainViewport();
	Require(closeMinimum.y >= viewport->WorkPos.y && closeMaximum.y <= viewport->WorkPos.y + viewport->WorkSize.y &&
				closeMaximum.x <= viewport->WorkPos.x + viewport->WorkSize.x,
			"Enlarged Preferences put Close outside the visible viewport");
	if (const auto directory = std::getenv("SIMXR_DIALOG_QA_DIR"))
	{
		test.renderer.RequestScreenshot(std::filesystem::path(directory) / "preferences-enlarged.bmp");
		test.Frame();
	}
	ImGui::ActivateItemByID(FindPanel("Workspace settings")->GetID("Close"));
	test.Frame();
	Require(!open && closed == 2, "Preferences Close did not report the completed edit session");
	test.extraDraw = {};
	test.workspace.SetScale(1);
	pump();
	std::cout << "PASS Preferences chords, capture cancellation and enlarged close controls\n";
}

void CheckTimelineWorkflow(Interaction& test)
{
	test.workspace.SetScale(1);
	test.workspace.Select(FrameSel{});
	const auto activate = [&](const char* label) {
		auto* timeline = FindPanel("Timeline");
		Require(timeline != nullptr, "Timeline missing");
		ImGui::ActivateItemByID(timeline->GetID(label));
		test.Frame();
	};
	const auto await = [&](auto predicate, const char* failure) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		do
		{
			test.Frame();
			if (predicate(test.runtime.Snapshot()))
				return;
		}
		while (std::chrono::steady_clock::now() < deadline);
		throw std::runtime_error(failure);
	};
	test.Frame();
	activate("Pause");
	await([](const auto& state) { return state.manual && !state.operationPending; }, "Timeline pause failed");
	RuntimeView earlier;
	for (int step = 0; step < 3; ++step)
	{
		const auto previous = test.runtime.Snapshot().selectedCapture;
		activate("Step forward");
		await(
			[&](const auto& state) {
				return !state.operationPending && state.selectedReady && state.selectedCapture != previous &&
					   state.inspectedFrame == state.selectedFrame;
			},
			"Timeline step did not retain image and CPU state");
		if (!step)
			earlier = test.runtime.Snapshot();
	}
	const auto newest = test.runtime.Snapshot();
	Require(newest.time.following, "Next frame stopped following the paused live edge");
	await(
		[&](const auto&) {
			return test.runtime.History().Find(earlier.selectedIdentity, earlier.selectedEpoch).has_value() &&
				   test.runtime.History().Find(newest.selectedIdentity, newest.selectedEpoch).has_value();
		},
		"Snapshot frame absent from timeline");
	Require(std::holds_alternative<FrameSel>(test.workspace.State().selection) &&
				std::get<FrameSel>(test.workspace.State().selection).identity == newest.selectedIdentity,
			"Next frame did not select the new frame");
	const auto index = *test.runtime.History().Find(earlier.selectedIdentity, earlier.selectedEpoch);
	Require(test.runtime.History().At(index).flags & HistorySnapshot, "Images lane omitted retained snapshot");
	test.Frame();
	const auto bounds = test.workspace.State().timelineCanvas;
	ImGui::GetIO().AddMousePosEvent(bounds.x + bounds.width - 1, bounds.y + 2);
	test.Frame();
	const auto& beforeZoom = test.workspace.State();
	const auto latestIndex = *test.runtime.History().Find(newest.selectedIdentity, newest.selectedEpoch);
	const double snapshotSpan =
		test.runtime.History().At(latestIndex).seconds - test.runtime.History().At(index).seconds;
	const double wantedSpan = std::max(.2, snapshotSpan * 1.5);
	const double visibleSpan = beforeZoom.timeline.last - beforeZoom.timeline.first;
	ImGui::GetIO().AddMouseWheelEvent(0, static_cast<float>(std::log(visibleSpan / wantedSpan) / std::log(1.5)));
	test.Frame();
	test.Frame();
	const auto& shell = test.workspace.State();
	const double seconds = test.runtime.History().At(index).seconds;
	const float x = shell.timelineCanvas.x +
					float((seconds - shell.timeline.first) / (shell.timeline.last - shell.timeline.first)) *
						shell.timelineCanvas.width;
	Require(x >= shell.timelineCanvas.x && x <= shell.timelineCanvas.x + shell.timelineCanvas.width,
			"Earlier snapshot not visible after timeline zoom");
	test.Click({x, shell.timelineCanvas.y + shell.timelineCanvas.height * .35f});
	await(
		[&](const auto& state) {
			return state.manual && !state.time.following && state.selectedFrame == earlier.selectedFrame &&
				   state.selectedReady && state.inspectedFrame == earlier.selectedFrame;
		},
		"Canvas did not select earlier image and inputs/actions");
	Require(std::holds_alternative<FrameSel>(test.workspace.State().selection),
			"Canvas selection did not open Frame page");
	auto* details = FindPanel("Frame details");
	Require(details != nullptr, "Merged Frame page absent");
	if (const auto directory = QaDirectory("SIMXR_UI_QA_DIR"); !directory.empty())
	{
		test.renderer.RequestScreenshot(directory / "timeline-paused.bmp");
		test.Frame();
		test.Frame();
		ImGui::ActivateItemByID(ImHashStr("Expand", 0, details->GetID("Recorded rig")));
		test.Frame();
		test.Frame();
		Require(test.runtime.Snapshot().rigExpanded, "Recorded rig did not expand into the preview");
		test.renderer.RequestScreenshot(directory / "frame-rig-expanded.bmp");
		test.Frame();
		auto* imageArea = FindPanel("Image area");
		Require(imageArea, "Expanded recorded rig image area missing");
		ImGui::ActivateItemByID(ImHashStr("Expand", 0, imageArea->GetID("Recorded rig")));
		test.Frame();
		test.Frame();
		Require(!test.runtime.Snapshot().rigExpanded, "Recorded rig did not restore the captured image");
		ImGui::SetScrollY(details, 410);
		test.Frame();
		test.Frame();
		test.renderer.RequestScreenshot(directory / "frame-input-actions.bmp");
		test.Frame();
		test.Frame();
	}
	const auto beforeRange = test.workspace.State();
	const auto rectangle = beforeRange.timelineCanvas;
	const float rangeStart = x - 1, rangeEnd = rectangle.x + rectangle.width - 1;
	const float rangeY = rectangle.y + ImGui::GetFrameHeight() / 2;
	auto& io = ImGui::GetIO();
	io.AddMousePosEvent(rangeStart, rangeY);
	test.Frame();
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	test.Frame();
	io.AddMousePosEvent(rangeEnd, rangeY);
	test.Frame();
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	test.Frame();
	test.Frame();
	Require(std::holds_alternative<RangeSel>(test.workspace.State().selection), "Ruler drag did not open Range page");
	auto* inspector = FindPanel("Inspector");
	ImGui::ActivateItemByID(inspector->GetID("Zoom to range"));
	test.Frame();
	test.Frame();
	Require(test.workspace.State().timeline.last - test.workspace.State().timeline.first <
				beforeRange.timeline.last - beforeRange.timeline.first,
			"Range zoom did not narrow the timeline");
	if (const auto directory = QaDirectory("SIMXR_UI_QA_DIR"); !directory.empty())
	{
		test.renderer.RequestScreenshot(directory / "timeline-range.bmp");
		test.Frame();
		test.Frame();
	}
	if (const auto directory = QaDirectory("SIMXR_UI_QA_DIR"); !directory.empty())
	{
		const auto setPath = [&](const std::filesystem::path& destination) {
			ImGui::ActivateItemByID(inspector->GetID("##Range export file"));
			ImGui::GetCurrentContext()->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
			test.Frame();
			io.AddKeyEvent(ImGuiMod_Ctrl, true);
			io.AddKeyEvent(ImGuiKey_A, true);
			test.Frame();
			io.AddKeyEvent(ImGuiKey_A, false);
			io.AddKeyEvent(ImGuiMod_Ctrl, false);
			io.AddInputCharactersUTF8(PathText(destination).c_str());
			test.Frame();
			io.AddKeyEvent(ImGuiKey_Enter, true);
			test.Frame();
			io.AddKeyEvent(ImGuiKey_Enter, false);
			test.Frame();
		};
		const auto invalid = directory / "range-missing-directory" / "frames.csv";
		Require(!std::filesystem::exists(invalid.parent_path()), "Invalid range export fixture already exists");
		setPath(invalid);
		ImGui::ActivateItemByID(inspector->GetID("Export frames"));
		test.Frame();

		struct CommaNumbers final : std::numpunct<char>
		{
			char do_decimal_point() const override
			{
				return ',';
			}
		};

		struct RestoreLocale
		{
			std::locale previous;

			~RestoreLocale()
			{
				std::locale::global(previous);
			}
		} restoreLocale{std::locale()};

		std::locale::global(std::locale(std::locale::classic(), new CommaNumbers));
		const auto destination = directory / Utf8Path("range-\u65e5\u672c\u8a9e.csv");
		setPath(destination);
		ImGui::ActivateItemByID(inspector->GetID("Export frames"));
		test.Frame();
		await(
			[&](const auto&) {
				std::size_t images = 0;
				for (const auto& file : std::filesystem::directory_iterator(directory))
					if (PathText(file.path().filename()).starts_with("range-\u65e5\u672c\u8a9e-frame-") &&
						file.path().extension() == ".png" && file.file_size() > 100)
						++images;
				const bool metadataReady = std::filesystem::exists(destination);
				if (!metadataReady)
					ImGui::ActivateItemByID(inspector->GetID("Export frames"));
				return metadataReady && images >= 3;
			},
			"Range export did not recover from the invalid destination and save three snapshots");
		Require(!std::filesystem::exists(invalid), "Failed range export reported a saved destination");
		std::ifstream metadata(destination);
		std::string row;
		std::size_t rows = 0;
		while (std::getline(metadata, row))
		{
			Require(std::count(row.begin(), row.end(), ',') == 8, "Range CSV used the UI numeric locale");
			++rows;
		}
		Require(metadata.eof() && rows > 3, "Range metadata was incomplete after export recovery");
	}
	activate("Continue");
	await([](const auto& state) { return !state.manual && state.time.following && !state.operationPending; },
		  "Continue did not return to LIVE");
	Require(test.runtime.Snapshot().error.empty(), "Timeline workflow reported runtime error");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0,
			"Timeline snapshot/thumbnail workflow produced Vulkan validation errors");
	std::cout << "PASS timeline pause, three steps, canvas click, historical image/input/actions, range "
				 "selection/zoom, Continue to LIVE\n";
}

void CheckShell()
{
	Interaction test{std::getenv("SIMXR_SHELL_QA_APP_NAME") != nullptr};
	test.live = true;
	const auto checkTimeline = [&] {
		const auto* viewport = ImGui::GetMainViewport();
		const auto* timeline = FindPanel("Timeline");
		const auto* preview = FindPanel("GPU preview");
		Require(timeline && preview, "Shell regions missing");
		Require(std::abs(timeline->Pos.x - viewport->WorkPos.x) < 1 &&
					std::abs(timeline->Pos.y + timeline->Size.y - viewport->WorkPos.y - viewport->WorkSize.y) < 1,
				"Timeline has outer left or bottom margins");
		Require(std::abs(timeline->Pos.x + timeline->Size.x - preview->Pos.x - preview->Size.x) < 1,
				"Timeline does not align with the preview's right edge");
		Require(!timeline->ScrollbarX && !timeline->ScrollbarY && timeline->ContentSize.x <= timeline->Size.x + 1,
				"Timeline controls overflow their region");
	};
	const auto checkToolbar = [&] {
		auto* toolbar = FindPanel("Top bar");
		const auto* viewport = ImGui::GetMainViewport();
		Require(toolbar && !toolbar->ScrollbarX && !toolbar->ScrollbarY &&
					toolbar->ContentSize.x <= toolbar->Size.x + 1 && toolbar->ContentSize.y <= toolbar->Size.y + 1,
				"Top-bar controls overflow their region");
		Require(std::abs(toolbar->Pos.x - viewport->WorkPos.x) < 1 &&
					std::abs(toolbar->Pos.y - viewport->WorkPos.y) < 1 &&
					std::abs(toolbar->Pos.x + toolbar->Size.x - viewport->WorkPos.x - viewport->WorkSize.x) < 1,
				"Top bar does not reach the window edges");
		for (const char* action : {"History", "Settings", "##App picker"})
		{
			ImGui::ActivateItemByID(toolbar->GetID(action));
			test.Frame();
			test.Frame();
			Require(!ImGui::GetCurrentContext()->OpenPopupStack.empty(), "Top-bar action did not open its popup");
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
			test.Frame();
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
			test.Frame();
			Require(ImGui::GetCurrentContext()->OpenPopupStack.empty(), "Top-bar popup did not close on Escape");
		}
	};
	const auto capture = [&](const char* name) {
		checkTimeline();
		if (const auto directory = QaDirectory("SIMXR_UI_QA_DIR"); !directory.empty())
		{
			std::filesystem::create_directories(directory);
			test.renderer.RequestScreenshot(directory / name);
			test.Frame();
		}
	};
	for (int i = 0; i < 4; ++i)
		test.Frame();
	checkToolbar();
	capture("shell-normal.bmp");
	const auto preview = FindPanel("GPU preview");
	const auto inspector = FindPanel("Inspector");
	Require(preview && inspector && !preview->DockId && !inspector->DockId, "Shell still uses docks");
	Require(preview->Pos.x + preview->Size.x <= inspector->Pos.x + 1, "Inspector overlaps the wide preview");
	const auto* viewport = ImGui::GetMainViewport();
	const auto* toolbar = FindPanel("Top bar");
	const auto* deviceTabs = FindPanel("Device tabs");
	Require(deviceTabs && std::abs(deviceTabs->Pos.y - toolbar->Pos.y - toolbar->Size.y) < 1 &&
				std::abs(deviceTabs->Pos.y + deviceTabs->Size.y - inspector->Pos.y) < 1 &&
				std::abs(inspector->Pos.x + inspector->Size.x - viewport->WorkPos.x - viewport->WorkSize.x) < 1 &&
				std::abs(inspector->Pos.y + inspector->Size.y - viewport->WorkPos.y - viewport->WorkSize.y) < 1,
			"Inspector is not joined to the toolbar and window edges");
	static constexpr const char* tabs[]{"Player", "Head", "Left", "Right", "Body", "Gaze", "Trackers"};
	for (const int index : {1, 2, 3, 4, 5, 6, 0})
	{
		auto* strip = FindPanel("Device tabs");
		Require(strip != nullptr, "Device tabs missing");
		ImGui::ActivateItemByID(strip->GetID(tabs[index]));
		test.Frame();
		test.Frame();
		const auto& selection = test.workspace.State().selection;
		Require(std::holds_alternative<DeviceSel>(selection) &&
					static_cast<int>(std::get<DeviceSel>(selection).page) == index,
				("Device tab selection failed at " + std::to_string(index)).c_str());
	}
	const auto width = preview->Size.x;
	ImGui::ActivateItemByID(FindPanel("Top bar")->GetID("Focus"));
	test.Frame();
	test.Frame();
	Require(FindPanel("GPU preview")->Size.x > width, "Full preview did not expand");
	ImGui::ActivateItemByID(FindPanel("Top bar")->GetID("Focus"));
	test.Frame();
	test.Frame();
	Require(std::abs(FindPanel("GPU preview")->Size.x - width) < 1, "Preview restore changed layout");
	ImGui::ActivateItemByID(FindPanel("Timeline divider tab")->GetID("##Edge"));
	for (int i = 0; i < 3; ++i)
		test.Frame();
	capture("shell-folded.bmp");
	ImGui::ActivateItemByID(FindPanel("Timeline divider tab")->GetID("##Edge"));
	test.Frame();
	test.window.Resize(800, 720);
	test.workspace.SetScale(1.5f);
	for (int i = 0; i < 6; ++i)
		test.Frame();
	checkToolbar();
	capture("shell-narrow-large.bmp");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Shell generated validation errors");
}

static void CheckCoreAssertions()
{
	{
		RuntimeView view;
		view.connected = view.sessionAvailable = true;
		Require(DeriveRunState(view, false, {}).state == RunState::WaitingForSession,
				"A session that has not started was labeled running");
		view.sessionRunning = true;
		Require(DeriveRunState(view, false, {}).state == RunState::Running,
				"A started session was not labeled running");
		view.manual = true;
		Require(DeriveRunState(view, false, {}).state == RunState::Paused, "A paused session was not labeled paused");
		view.sessionRunning = false;
		Require(DeriveRunState(view, false, {}).state == RunState::WaitingForSession,
				"A stopped session retained its paused state");
	}
	{
		simxr::app::LaunchProfile source;
		source.name = std::string(200, 'n');
		source.executable = "/" + std::string(3000, 'x');
		source.workingDirectory = source.runtimeManifest = source.rig = source.executable;
		source.privateProtonPrefix = "Z:\\private 日本語\\pfx";
		source.headsetProfile = std::string(300, 'h');
		source.arguments = {"", "first", "", "line one\nline two", std::string(18000, 'a'), ""};
		source.environment = {{std::string(300, 'K'), std::string(5000, 'v')}, {"EMPTY", ""}};
		simxr::app::CompanionSettings settings;
		settings.profiles.push_back(source);
		simxr::app::LaunchProfilesDialog editor;
		editor.Edit(settings, 0);
		const auto check = [&](const auto& actual) {
			Require(actual.name == source.name && actual.executable == source.executable &&
						actual.arguments == source.arguments && actual.environment == source.environment &&
						actual.workingDirectory == source.workingDirectory &&
						actual.runtimeManifest == source.runtimeManifest && actual.rig == source.rig &&
						actual.headsetProfile == source.headsetProfile &&
						actual.privateProtonPrefix == source.privateProtonPrefix,
					"Launch editor truncated stored fields or changed argument boundaries");
		};
		check(editor.Profile());
		source.headsetProfile.clear(); // Saving requires a known headset profile or the default.
		settings.profiles.clear();
		Require(editor.Create(settings, source), "Long launch profile could not be saved");
		check(settings.profiles.front());
		editor.Edit(settings, static_cast<int>(settings.profiles.size()));
		simxr::app::LaunchProfile newProfile;
		newProfile.useBundledRuntime = true;
		Require(editor.Selected() == -1 && editor.Profile() == newProfile,
				"A retired launch-profile index did not open a new draft");
	}
}

void CheckDefaultInteraction()
{
	CheckCoreAssertions();
	auto validation = std::make_shared<ValidationLog>();
	{
		Interaction test;
		validation = test.validation;
		for (unsigned frame = 0; frame < 4; ++frame)
			test.Frame();
		test.workspace.ShowLibrary(true);
		test.CheckControls();
		test.SelectWorkspace(DeviceSel{});
		test.SelectWorkspace(FrameSel{});
		for (const auto size : {ImVec2{800, 600}, ImVec2{1024, 700}, ImVec2{900, 640}})
		{
			const auto rebuilds = test.renderer.RebuildCount();
			test.window.Resize(static_cast<int>(size.x), static_cast<int>(size.y));
			Eventually(
				[&] {
					test.Frame();
					return test.renderer.RebuildCount() > rebuilds;
				},
				"Resize did not rebuild the swapchain");
			test.CheckControls();
		}
		{
			CompanionSettings profiles;
			profiles.profiles = {{"One", "/missing-one"}, {"Two##literal", "/missing-two"}};
			LaunchProfilesDialog editor;
			editor.Edit(profiles, 0);
			bool open = true, changed = false;
			test.extraDraw = [&] {
				ImGui::Begin("Launch editor fixture");
				editor.Draw(profiles, {}, open, changed);
				ImGui::End();
			};
			for (int frame = 0; frame < 3; ++frame)
				test.Frame();
			auto* form = FindPanel("Profile form");
			Require(form, "Launch profile form missing");
			ImGui::ActivateItemByID(ImHashStr("##Name", 0, ImHashStr("1", 0, form->ID)));
			test.Frame();
			auto& io = ImGui::GetIO();
			io.AddKeyEvent(ImGuiMod_Ctrl, true);
			io.AddKeyEvent(ImGuiKey_A, true);
			test.Frame();
			io.AddKeyEvent(ImGuiKey_A, false);
			io.AddKeyEvent(ImGuiMod_Ctrl, false);
			test.Frame();
			const auto longName = std::string(1800, 'n') + " Ångström";
			io.AddInputCharactersUTF8(longName.c_str());
			test.Frame();
			Require(changed && profiles.profiles[0].name == longName && ImGui::GetActiveID() != 0,
					"Launch profile typing was truncated or waited for blur before saving");
			auto* names = FindPanel("Profile names");
			Require(names, "Launch profile list missing");
			test.Click({names->DC.CursorStartPos.x + 20,
						names->DC.CursorStartPos.y + ImGui::GetTextLineHeightWithSpacing() * 1.5f});
			Require(editor.Selected() == 1 && editor.Profile().name == "Two##literal" &&
						profiles.profiles[0].name == longName,
					"Switching launch profiles lost the outgoing edit or replaced the new form");
			profiles.profiles.insert(profiles.profiles.begin(), {"Inserted", "/missing-inserted"});
			test.Frame();
			Require(editor.Selected() == 2 && editor.Profile().name == "Two##literal",
					"Launch editor followed a shifted profile index");
			test.workspace.SetScale(2);
			for (int frame = 0; frame < 3; ++frame)
				test.Frame();
			if (const auto directory = std::getenv("SIMXR_DIALOG_QA_DIR"))
			{
				test.renderer.RequestScreenshot(std::filesystem::path(directory) / "launch-profiles.bmp");
				test.Frame();
			}
			test.workspace.SetScale(1);
			profiles.profiles.erase(profiles.profiles.begin() + 2);
			test.Frame();
			Require(editor.Selected() == -1 && profiles.profiles.size() == 2 &&
						profiles.profiles[0].name == "Inserted" && profiles.profiles[1].name == longName,
					"Removing the edited profile overwrote another profile");
			open = false;
			test.Frame();
			test.extraDraw = {};
			std::cout << "PASS launch editor live text, profile switching and retained selection identity\n";
		}
		{
			RecordingPanel recording;
			recording.SetTarget({"retired-recording-target", 1, 1});
			recording.RequestStart();
			recording.SetTarget({"replacement-recording-target", 2, 1});
			test.extraDraw = [&] {
				ImGui::Begin("Recording scope probe");
				recording.DrawPopovers(true);
				ImGui::End();
			};
			test.Frame();
			const auto state = recording.Snapshot();
			Require(!state.busy && state.error.empty(), "Stale recording click reached the replacement target");
			recording.RequestStart();
			recording.RequestStop();
			recording.Poll(true);
			test.Frame();
			const auto stopped = recording.Snapshot();
			Require(!stopped.busy && stopped.error.empty(),
					"An earlier Start or background read overrode the queued Stop");
			test.extraDraw = {};
		}
		CheckNotices(test);
		CheckPreferences(test);
		CheckTimelineCanvas(test);
		test.CheckFocusLoss();
		test.renderer.WaitIdle();
	}
	Require(validation->errors.load() == 0, "Vulkan validation errors during interaction or teardown");
	CheckShell();
#ifdef __linux__
	CheckApplicationTabs();
#endif
	ConfigureObservedDevices();
	CheckSpatialWidgetLifecycle();
	CheckDocumentTreeIdentity();
	CheckCapturedRig();
	if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
		CaptureLayouts(directory);
	std::cout << "PASS fixed shell, rail selection and SDL input across three resizes\n";
}

#ifdef __linux__
void CheckDiscoveryFailure()
{
	Interaction test;
	const auto notices = test.runtime.TakeNotices();
	Require(std::any_of(notices.begin(), notices.end(),
						[](const auto& notice) {
							return notice.title == "Application discovery failed" && !notice.detail.empty();
						}),
			"Discovery failure did not become a notice during startup");
	AppPicker picker;
	std::string failure;
	bool open = true;
	test.extraDraw = [&] {
		ImGui::Begin("Discovery failure fixture");
		const auto choice = picker.Draw({}, {}, "Apps", 0, "", std::exchange(open, false), {});
		if (!choice.discoveryError.empty())
			failure = choice.discoveryError;
		ImGui::End();
	};
	Eventually(
		[&] {
			test.Frame();
			return !failure.empty();
		},
		"Picker hid a discovery failure");
	Require(!failure.empty(), "Picker hid a discovery failure");
	if (const auto path = std::getenv("SIMXR_DISCOVERY_QA_SCREENSHOT"))
	{
		test.renderer.RequestScreenshot(path);
		test.Frame();
	}
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Discovery failure UI validation errors");
	std::cout << "PASS startup notice and usable app picker after discovery failure\n";
}
#endif

void CheckShellScenario()
{
	if (const auto name = std::getenv("SIMXR_SHELL_QA_APP_NAME"))
	{
		const auto path = QaDirectory("SIMXR_COMPANION_SETTINGS");
		Require(!path.empty() && !std::filesystem::exists(path), "Named shell QA needs a new private settings file");
		CompanionSettings settings;
		settings.profiles.push_back({name, "missing-qa-app"});
		settings.activeProfile = name;
		SaveSettings(path, settings);
	}
	CheckShell();
	std::cout << "PASS fixed shell, toolbar actions, focus restore and timeline edges at normal, folded and narrow "
				 "enlarged layouts\n";
}

}
