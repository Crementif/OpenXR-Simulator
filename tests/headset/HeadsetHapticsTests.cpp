#include "../TestSupport.h"
#include "Pch.h"
#include "headset/HeadsetHaptics.h"

#include <iostream>

namespace simxr::tests
{
static void Forwarding()
{
	using namespace std::chrono_literals;
	headset::HeadsetHaptics state;
	const headset::HeadsetHaptics::Clock::time_point now{1s};
	control::HapticObservation observation{
		100, false, {{1, {InputDeviceKind::Controller, 0}, .5f, 80, 0, {}, HapticStatus::Active}}};
	Require(state.Observe(observation, now, now) && !state.Consume(now, true)[0], "Initial history must not vibrate");
	observation.events[0].sequence = 2;
	observation.events[0].start = observation.time;
	observation.events[0].end = observation.time + 100'000'000;
	Require(state.Observe(observation, now, now + 10ms), "Accept new vibration");
	const auto first = state.Consume(now + 20ms, true);
	Require(first[0] && first[0]->remaining == 80'000'000 && first[0]->frequency == 80 && !first[1],
			"Account for transport and consumption delay without affecting the other hand");
	Require(state.Observe(observation, now + 20ms, now + 20ms) && !state.Consume(now + 30ms, true)[0],
			"Repeated observation cannot restart a pulse");
	const auto expired = state.Consume(now + 101ms, true);
	Require(expired[0] && expired[0]->amplitude == 0, "Repeated observation cannot extend original deadline");
	observation.events[0].sequence = 3;
	observation.events[0].end.reset();
	Require(state.Observe(observation, now + 110ms, now + 110ms), "Accept replacement");
	Require(state.Consume(now + 110ms, true)[0]->remaining == 2'000'000'000, "Bound indefinite events");
	Require(state.Consume(now + 111ms, false)[0]->amplitude == 0, "Disable stops output");
	state.Observe(observation, now + 112ms, now + 112ms);
	Require(!state.Consume(now + 112ms, true)[0], "Re-enable cannot replay an old event");
	observation.events[0].sequence = 4;
	state.Observe(observation, now + 120ms, now + 120ms);
	Require(state.Consume(now + 120ms, true)[0].has_value(), "New event resumes output");
	observation.events[0].status = HapticStatus::Stopped;
	state.Observe(observation, now + 121ms, now + 121ms);
	Require(state.Consume(now + 121ms, true)[0]->amplitude == 0, "Same-sequence stop reaches output");
	observation.events[0].sequence = 5;
	observation.events[0].status = HapticStatus::Active;
	state.Observe(observation, now + 122ms, now + 122ms);
	state.Consume(now + 122ms, true);
	observation.overflow = true;
	Require(!state.Observe(observation, now + 123ms, now + 123ms) &&
				state.Consume(now + 123ms, true)[0]->amplitude == 0,
			"Overflow suspends and stops output");
	observation.overflow = false;
	Require(!state.Observe(observation, now + 124ms, now + 124ms), "Failed attachment cannot silently resume");
}

static void StaleAndDelayed()
{
	using namespace std::chrono_literals;
	headset::HeadsetHaptics state;
	const headset::HeadsetHaptics::Clock::time_point now{1s};
	control::HapticObservation observation{100, false, {}};
	state.Observe(observation, now, now);
	observation.events.push_back({1, {InputDeviceKind::Controller, 1}, .5f, 0, 100, 100'000'100, HapticStatus::Active});
	state.Observe(observation, now, now + 101ms);
	Require(!state.Consume(now + 101ms, true)[1], "Transport delay cannot replay an already expired pulse");
	observation.events[0].sequence = 2;
	observation.events[0].end.reset();
	state.Observe(observation, now + 110ms, now + 110ms);
	const auto started = state.Consume(now + 110ms, true);
	Require(!started[0] && started[1], "Right output is independent");
	const auto stale = state.Consume(now + 361ms, true);
	Require(stale[1] && stale[1]->amplitude == 0, "Missing observations stop output within freshness bound");
	state.Observe(observation, now + 362ms, now + 362ms);
	Require(!state.Consume(now + 362ms, true)[1], "A delayed repeat cannot resume a stale pulse");
	observation.time = 99;
	Require(!state.Observe(observation, now + 363ms, now + 363ms), "Clock reversal requires a new attachment");
}

static void SuspendedEvents()
{
	using namespace std::chrono_literals;
	headset::HeadsetHaptics state;
	const headset::HeadsetHaptics::Clock::time_point now{1s};
	control::HapticObservation observation{100, false, {}};
	state.Observe(observation, now, now);
	observation.events.push_back({1, {InputDeviceKind::Controller, 0}, .5f, 0, 100, {}, HapticStatus::Active});
	state.Observe(observation, now, now);
	Require(state.Consume(now, true)[0].has_value(), "Start pulse before suspension");
	observation.events[0].sequence = 2;
	Require(state.Observe(observation, now + 1ms, now + 1ms, false), "Observe suspended haptic watermark");
	const auto stopped = state.Consume(now + 2ms, false);
	Require(stopped[0] && stopped[0]->amplitude == 0, "Suspension retains physical stop notification");
	state.Observe(observation, now + 3ms, now + 3ms);
	Require(!state.Consume(now + 3ms, true)[0], "Resume never replays a pulse observed during suspension");
	observation.events[0].sequence = 3;
	state.Observe(observation, now + 4ms, now + 4ms);
	Require(state.Consume(now + 4ms, true)[0].has_value(), "Fresh post-resume pulse remains available");
}

} // namespace simxr::tests

int main()
{
	try
	{
		simxr::tests::Forwarding();
		simxr::tests::StaleAndDelayed();
		simxr::tests::SuspendedEvents();
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
