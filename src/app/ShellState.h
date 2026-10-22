#pragma once

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
enum class SessionItem
{
	Headset,
	Scenarios
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
struct SessionItemSel
{
	SessionItem item = SessionItem::Headset;
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
struct LibrarySel
{
};
struct AdvancedSel
{
};
using Selection = std::variant<std::monostate, DeviceSel, SessionItemSel, FrameSel, RangeSel, TakeSel, ScriptSel,
							   LibrarySel, AdvancedSel>;
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
	// Set by an empty-state button; the top bar opens its app picker.
	bool openAppPicker = false;
};
// Returning to the live edge ends a frame selection (and, with range, a range
// selection) and shows the device page that was open before.
void LeaveHistory(ShellState& shell, bool range);
} // namespace simxr::app
