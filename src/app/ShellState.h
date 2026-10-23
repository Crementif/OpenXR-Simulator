#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace simxr::app
{
enum class DevicePage
{
	Player,
	Head,
	Left,
	Right,
	Body,
	Gaze,
	Trackers
};
struct FrameIdentity
{
	std::uint64_t session = 0, run = 0, frame = 0;
	bool operator==(const FrameIdentity&) const = default;
};
struct DeviceSel
{
	DevicePage page = DevicePage::Player;
	std::uint32_t slot = 0;
};
struct FrameSel
{
	FrameIdentity identity;
	std::uint64_t epoch = 0;
};
struct RangeSel
{
	FrameIdentity first, last;
	std::uint64_t epoch = 0;
};
struct TakeSel
{
	std::string id;
};
struct ScriptSel
{
	std::string id;
};
struct AdvancedSel
{
};
using Selection = std::variant<std::monostate, DeviceSel, FrameSel, RangeSel, TakeSel, ScriptSel, AdvancedSel>;
// Which frame the app shows. Written only by RuntimeSession: following shows
// the live edge (the paused frame while paused); otherwise the cursor frame.
struct TimeCursor
{
	bool following = true;
	FrameIdentity cursor;
	std::uint64_t epoch = 0;
};
// Where a take cut from history came from, so its replay can show the source.
struct TakeSource
{
	std::string path;
	FrameIdentity first, last;
	std::uint64_t epoch = 0;
};
// The timeline's visible span, owned by the timeline.
struct TimelineWindow
{
	double first = 0, last = 0;
	bool zoomToRange = false;
	std::optional<TakeSource> savedTake;
	// A Rec take just saved; the timeline attaches it to its Recording clip.
	std::optional<std::string> recordedTake;
	// A take playing on the selected session. The transport's Pause then sets
	// togglePlayback instead of pausing the app, and the shell pauses the take.
	bool playing = false, playbackPaused = false, togglePlayback = false;
	// The Stop on a running replay's band; the shell cancels the replay.
	bool stopReplay = false;
	// When the running recording started, for its elapsed time.
	std::optional<std::chrono::steady_clock::time_point> recordingSince;
};
enum class RunState
{
	NotConnected,
	Launching,
	WaitingForSession,
	Running,
	Paused,
	Disconnected,
	Failed
};
struct SessionState
{
	RunState state = RunState::NotConnected;
	std::string endpoint, error;
	std::uint64_t session = 0, generation = 0, run = 0;
	float fps = 0;
};
struct RuntimeView;
SessionState DeriveRunState(const RuntimeView& view, bool launching, const std::string& launchError);
const char* RunStateLabel(RunState state);
struct ShellRect
{
	float x = 0, y = 0, width = 0, height = 0;
};
struct ShellState
{
	SessionState session;
	Selection selection = DeviceSel{};
	// The device page to return to when a frame or range selection ends.
	Selection device = DeviceSel{};
	TimelineWindow timeline;
	ShellRect preview, inspector, timelineCanvas;
	bool inspectorVisible = true;
	// The Library drawer in the timeline's left column is open.
	bool library = false;
	// Set by an empty-state button; the top bar opens its app picker.
	bool openAppPicker = false;
};
// Returning to the live edge ends a frame selection (and, with range, a range
// selection) and shows the device page that was open before.
void LeaveHistory(ShellState& shell, bool range);
} // namespace simxr::app
