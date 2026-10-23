#include "../TestSupport.h"
#include "app/services/HapticFeedback.h"
#include "control/ControlSchema.h"

#include <iostream>

using namespace simxr;
using namespace simxr::app;
using namespace std::chrono_literals;
namespace wire = simxr::control;

static wire::HapticObservation Observation(std::uint64_t sequence = 1, std::int64_t time = 0)
{
	return {time,
			false,
			{{sequence, {InputDeviceKind::Controller, 0}, .5f, 120, 0, {}, HapticStatus::Active},
			 {sequence + 1, {InputDeviceKind::Controller, 1}, 1, 0, 0, 1'000'000'000, HapticStatus::Active}}};
}
static void Feedback()
{
	HapticFeedback feedback;
	const auto start = HapticFeedback::Clock::time_point{} + 10s;
	feedback.Observe(Observation(), start, start);
	Require(feedback.Motors(start, true) == std::array<std::uint16_t, 2>{32768, 65535}, "Map amplitude to each motor");
	for (int i = 1; i <= 21; ++i)
	{
		const auto now = start + i * 100ms;
		feedback.Observe(Observation(), now, now); // Simulated time is paused; refresh must not extend deadlines.
		const auto motors = feedback.Motors(now, true);
		Require(motors[0] == (i < 20 ? 32768 : 0) && motors[1] == (i < 10 ? 65535 : 0),
				"Repeated observations extended a finite or two-second rumble deadline");
	}
	feedback.Observe(Observation(3), start + 3s, start + 3s);
	Require(feedback.Motors(start + 3251ms, true) == std::array<std::uint16_t, 2>{},
			"Stale observations stop both motors");
	feedback.Observe(Observation(3), start + 3300ms, start + 3300ms);
	Require(feedback.Motors(start + 3300ms, true) == std::array<std::uint16_t, 2>{}, "Stale events must not restart");
	feedback.Observe(Observation(5), start + 4s, start + 4s);
	feedback.Motors(start + 4s, false);
	feedback.Observe(Observation(5), start + 4100ms, start + 4100ms);
	Require(feedback.Motors(start + 4100ms, true) == std::array<std::uint16_t, 2>{},
			"Focus/device loss must not replay old output");
	auto stopped = Observation(7);
	feedback.Observe(stopped, start + 5s, start + 5s);
	stopped.events[0].status = HapticStatus::Stopped;
	stopped.events[1].status = HapticStatus::FocusLost;
	feedback.Observe(stopped, start + 5010ms, start + 5010ms);
	Require(feedback.Motors(start + 5010ms, true) == std::array<std::uint16_t, 2>{},
			"Stop and focus loss retire output immediately");
	feedback.Reset();
	feedback.Observe(Observation(), start, start + 3s);
	Require(feedback.Motors(start + 3s, true) == std::array<std::uint16_t, 2>{},
			"Late replies must not start expired vibration");
	feedback.Reset();
	feedback.Observe(Observation(), start, start);
	Require(feedback.Motors(start, true)[0] != 0, "A new session may reuse sequence identities");
}
static void ShortPulses()
{
	HapticFeedback feedback;
	const auto start = HapticFeedback::Clock::time_point{} + 10s;
	auto pulse = Observation();
	pulse.events.resize(1);
	pulse.events[0].status = HapticStatus::Expired;
	pulse.events[0].end = 1'000'000;
	pulse.time = 20'000'000;
	feedback.Observe(pulse, start, start);
	Require(feedback.Motors(start, true)[0] == 0, "Initial history must not replay expired pulses");
	pulse.events[0].sequence = 2;
	feedback.Observe(pulse, start + 10ms, start + 10ms);
	Require(feedback.Motors(start + 10ms, true)[0] == 32768 && feedback.Deadline() == start + 11ms,
			"A short event ending between observations must produce one bounded pulse");
	feedback.Observe(pulse, start + 12ms, start + 12ms);
	Require(feedback.Motors(start + 12ms, true)[0] == 0, "Repeated expired event restarted its pulse");
	pulse.events[0].sequence = 3;
	feedback.Observe(pulse, start + 1s, start + 1s);
	Require(feedback.Motors(start + 1s, true)[0] == 0, "Reconnection after a gap replayed short history");
}
static void Malformed()
{
	const std::array output{HapticOutput{
		1, {InputDeviceKind::Controller, 0}, XR_NULL_HANDLE, XR_NULL_PATH, .5f, 0, XR_INFINITE_DURATION, {}, {}}};
	wire::Message message;
	message.fields = {wire::Field::I64(wire::tag::Time, 0), wire::Field::Bool(wire::tag::Overflow, false),
					  wire::EncodeHaptics(output)};
	Require(wire::DecodeHaptics(message).events.size() == 1, "Decode bounded output observation");
	message.fields.back().value.pop_back();
	bool rejected = false;
	try
	{
		wire::DecodeHaptics(message);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Malformed haptic data must not drive hardware");
}
int main()
{
	try
	{
		Feedback();
		ShortPulses();
		Malformed();
		std::cout << "PASS live haptic timing, stop, stale/focus/device retirement and wire validation\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
