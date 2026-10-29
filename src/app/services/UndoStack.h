#pragma once

#include "app/services/SimulationEdits.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace simxr::app
{
// Ctrl+Z history of device poses: one entry per edit group (a whole drag or one click), never per frame.
class UndoStack
{
public:
	struct Change
	{
		std::string label;
		std::chrono::steady_clock::time_point time;
	};

	void Record(const ObservedSetup& before, const ManualTransaction& edit);

	void FinishGesture()
	{
		m_open = false;
	}

	void Observe(const ObservedSetup& observed);

	// Restores the channels authored by the newest groups; blocked ownership keeps history intact.
	std::optional<ManualTransaction> Undo(const ObservedSetup& current, std::optional<CommandSource> source,
										  std::size_t steps = 1);
	void Clear();

	bool Empty() const
	{
		return m_entries.empty();
	}

	std::vector<Change> Changes() const;

private:
	struct State
	{
		TrackingState tracking;
		BodyProportions proportions;
	};

	struct Entry
	{
		State before;
		std::array<bool, ControlChannelCount> channels{};
		std::array<bool, MaxTrackerDevices> trackers{};
		std::array<TrackerIdentity, MaxTrackerDevices> trackerIdentities{};
		std::array<TrackerSourcePath, MaxTrackerDevices> trackerSources{};
		bool trackerCollection = false;
		std::chrono::steady_clock::time_point time;
		std::string label;
		std::string description;
	};

	static std::string Describe(const State& before, const State& after);

	std::vector<Entry> m_entries;
	std::optional<State> m_authored;
	bool m_open = false;
};
} // namespace simxr::app
