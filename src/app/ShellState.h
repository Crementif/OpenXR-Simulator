#pragma once

#include <cstdint>
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
};
struct RangeSel
{
	FrameIdentity first, last;
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
struct TimeCursor
{
	bool following = true;
	FrameIdentity cursor;
	double first = 0, last = 0, zoom = 1;
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
struct RuntimePanelSnapshot;
SessionState DeriveRunState(const RuntimePanelSnapshot& snapshot, bool launching, const std::string& error);
const char* RunStateLabel(RunState state);
struct ShellRect
{
	float x = 0, y = 0, width = 0, height = 0;
};
struct ShellState
{
	SessionState session;
	Selection selection = DeviceSel{};
	TimeCursor time;
	ShellRect preview, inspector;
	bool inspectorVisible = true;
};
} // namespace simxr::app
