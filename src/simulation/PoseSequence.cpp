#include "simulation/PoseSequence.h"
#include "Pch.h"
#include "Json.h"
#include "math/Math.h"
#include "simulation/InputState.h"

namespace simxr
{
template<class T, std::size_t N>
static bool Any(const std::array<std::optional<T>, N>& values)
{
	return std::any_of(values.begin(), values.end(), [](const auto& value) { return value.has_value(); });
}

static void Position(XrVector3f& value, const std::array<std::optional<float>, 3>& patch)
{
	value = {patch[0].value_or(value.x), patch[1].value_or(value.y), patch[2].value_or(value.z)};
}

void HeadPosePatch::Apply(XrPosef& pose, std::array<float, 3>& currentAngles) const
{
	Position(pose.position, position);
	if (Any(angles))
	{
		for (std::size_t axis = 0; axis < angles.size(); ++axis)
			currentAngles[axis] = angles[axis].value_or(currentAngles[axis]);
		pose.orientation = EulerAngles(currentAngles[0], currentAngles[1], currentAngles[2]);
	}
}

void ControllerPosePatch::Apply(XrPosef& pose, ControllerControls& controls) const
{
	Position(pose.position, position);
	if (orientation)
		pose.orientation = *orientation;
	if (trigger)
	{
		controls.trigger = std::clamp(*trigger, 0.0f, 1.0f);
		controls.triggerTouch = controls.trigger > 0;
	}
	if (grip)
		controls.gripForce = controls.grip = std::clamp(*grip, 0.0f, 1.0f);
	if (stick[0])
		controls.trackpad.x = controls.stick.x = std::clamp(*stick[0], -1.0f, 1.0f);
	if (stick[1])
		controls.trackpad.y = controls.stick.y = std::clamp(*stick[1], -1.0f, 1.0f);
	if (primary)
		controls.primaryTouch = controls.primary = *primary;
	if (secondary)
		controls.secondaryTouch = controls.secondary = *secondary;
}

static std::optional<float> ReadNumber(const json::Object& document, const char* name)
{
	if (!document.Has(name))
		return {};
	const float value = document.Number(name, std::numeric_limits<float>::quiet_NaN());
	return std::isfinite(value) ? std::optional{value} : std::nullopt;
}

static float NumberOr(const json::Object& document, const char* name, float fallback)
{
	return ReadNumber(document, name).value_or(fallback);
}

static std::optional<bool> ReadButton(const json::Object& document, const char* name)
{
	if (!document.Has(name))
		return {};
	const bool whenFalse = document.Boolean(name, false);
	if (whenFalse == document.Boolean(name, true))
		return whenFalse;
	const auto number = ReadNumber(document, name);
	return number ? std::optional{*number != 0} : std::nullopt;
}

static std::optional<std::uint64_t> SequenceFrame(const json::Object& document)
{
	if (!document.Has("frame"))
		return 0;
	auto text = document.Raw("frame");
	if (text.empty())
		return {};
	const bool quoted = text.size() >= 2 && text.front() == '"' && text.back() == '"';
	if (quoted)
		text = text.substr(1, text.size() - 2);
	if (text.empty())
		return {};
	std::uint64_t frame = 0;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), frame);
	if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
		return frame;
	if (quoted)
		return {};
	// Preserve legacy fractional/exponent inputs only where double conversion is exact enough.
	const double value = document.Number("frame", std::numeric_limits<double>::quiet_NaN());
	if (!std::isfinite(value) || value > 9007199254740991.0)
		return {};
	return static_cast<std::uint64_t>(std::max(0.0, value));
}

template<class Head, class Controller>
static auto TrackSlot(Head* head, std::array<Controller*, 2> controllers, std::size_t track)
	-> decltype(&head->position[0])
{
	if (track < 6)
		return !head ? nullptr : track < 3 ? &head->position[track] : &head->angles[track - 3];
	auto* controller = controllers[(track - 6) / 7];
	if (!controller)
		return nullptr;
	switch (const auto component = (track - 6) % 7)
	{
	case 3:
		return &controller->trigger;
	case 4:
		return &controller->grip;
	case 5:
	case 6:
		return &controller->stick[component - 5];
	default:
		return &controller->position[component];
	}
}

static const std::optional<float>* TrackSlot(const PoseSequenceStep& step, std::size_t track)
{
	const auto pointer = [](const auto& value) {
		return value ? &*value : nullptr;
	};
	return TrackSlot(pointer(step.head), std::array{pointer(step.controllers[0]), pointer(step.controllers[1])}, track);
}

static std::optional<float>* TrackSlot(PoseSequenceSample& sample, std::size_t track)
{
	return TrackSlot(&sample.head, std::array{&sample.controllers[0], &sample.controllers[1]}, track);
}

template<class Key, class Value, class Mix>
static Value Interpolate(const std::vector<Key>& keys, Duration elapsed, Value start, Mix mix)
{
	const auto next = std::upper_bound(keys.begin(), keys.end(), elapsed,
									   [](Duration time, const Key& key) { return time < key.time; });
	if (next == keys.end())
		return keys.back().value;
	const auto from = next == keys.begin() ? Duration{} : std::prev(next)->time;
	const auto value = next == keys.begin() ? start : std::prev(next)->value;
	return mix(value, next->value,
			   static_cast<double>((elapsed - from).count()) / static_cast<double>((next->time - from).count()));
}

static XrQuaternionf Slerp(XrQuaternionf from, XrQuaternionf to, double fraction)
{
	double dot = from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w;
	if (dot < 0)
	{
		to = {-to.x, -to.y, -to.z, -to.w};
		dot = -dot;
	}
	double weightFrom = 1 - fraction, weightTo = fraction;
	if (dot < 0.9995)
	{
		const double angle = std::acos(dot), sine = std::sin(angle);
		weightFrom = std::sin((1 - fraction) * angle) / sine;
		weightTo = std::sin(fraction * angle) / sine;
	}
	const auto mix = [&](float a, float b) {
		return weightFrom * a + weightTo * b;
	};
	const double x = mix(from.x, to.x), y = mix(from.y, to.y), z = mix(from.z, to.z), w = mix(from.w, to.w);
	const double length = std::sqrt(x * x + y * y + z * z + w * w);
	if (!(length > 0))
		return to;
	return {static_cast<float>(x / length), static_cast<float>(y / length), static_cast<float>(z / length),
			static_cast<float>(w / length)};
}

static HeadPosePatch DecodeHead(const json::Object& document)
{
	return {{ReadNumber(document, "x"), ReadNumber(document, "y"), ReadNumber(document, "z")},
			{ReadNumber(document, "yaw"), ReadNumber(document, "pitch"), ReadNumber(document, "roll")}};
}

static ControllerPosePatch DecodeController(const json::Object& document)
{
	ControllerPosePatch update;
	update.position = {ReadNumber(document, "posX"), ReadNumber(document, "posY"), ReadNumber(document, "posZ")};
	if (document.Has("yaw") || document.Has("pitch") || document.Has("roll"))
		update.orientation =
			EulerAngles(NumberOr(document, "yaw", 0), NumberOr(document, "pitch", 0), NumberOr(document, "roll", 0));
	update.trigger = ReadNumber(document, "trigger");
	update.grip = ReadNumber(document, "grip");
	update.stick = {ReadNumber(document, "stickX"), ReadNumber(document, "stickY")};
	update.primary = ReadButton(document, "primary");
	update.secondary = ReadButton(document, "secondary");
	return update;
}

PoseSequence PoseSequence::Parse(std::string_view source, bool loop)
{
	std::vector<PoseSequenceStep> sequence;
	std::size_t timedRows = 0;
	std::string line;
	while (!source.empty())
	{
		const auto end = source.find('\n');
		line.assign(source.substr(0, end));
		source.remove_prefix(end == std::string_view::npos ? source.size() : end + 1);
		const json::Object document(line.c_str());
		if (!document.Valid())
			continue;
		PoseSequenceStep step;
		if (document.Has("time"))
		{
			if (document.Has("frame"))
				throw std::invalid_argument("A pose sequence row selects both a frame and a time");
			const double seconds = document.Number("time", std::numeric_limits<double>::quiet_NaN());
			if (!(seconds >= 0) || seconds > std::chrono::duration<double>(PoseSequence::MaxTime).count())
				throw std::invalid_argument("Pose sequence time must be a nonnegative number of seconds");
			step.time = std::chrono::duration_cast<Duration>(std::chrono::duration<double>(seconds));
			++timedRows;
		}
		else
		{
			const auto frame = SequenceFrame(document);
			if (!frame)
				continue;
			step.frame = {*frame};
		}
		if (sequence.size() == PoseSequence::MaxSteps)
			throw std::length_error("Pose sequence exceeds its step limit");
		const auto head = document.Child("head");
		if (head.Valid())
			step.head = DecodeHead(head);
		for (std::size_t hand = 0; hand < step.controllers.size(); ++hand)
		{
			const auto controller = document.Child(hand ? "right" : "left");
			if (controller.Valid())
				step.controllers[hand] = DecodeController(controller);
		}
		step.capture = document.Boolean("capture", false);
		sequence.push_back(std::move(step));
	}
	if (timedRows && timedRows != sequence.size())
		throw std::invalid_argument("Pose sequence rows must all select a time or all select a frame");
	return {std::move(sequence), loop, timedRows != 0};
}

PoseSequence::PoseSequence(std::vector<PoseSequenceStep> steps, bool loop, bool timed)
	: m_steps(std::move(steps)), m_loop(loop), m_timed(timed)
{
	if (m_steps.size() > MaxSteps)
		throw std::length_error("Pose sequence exceeds its step limit");
	for (const auto& step : m_steps)
	{
		if (step.head && (Any(step.head->position) || Any(step.head->angles)))
			m_channels[0] = m_channels[1] = m_channels[2] = true;
		for (std::size_t hand = 0; hand < step.controllers.size(); ++hand)
			if (const auto& patch = step.controllers[hand])
			{
				m_channels[hand + 1] = m_channels[hand + 1] || Any(patch->position) || patch->orientation.has_value();
				m_channels[hand + 3] = m_channels[hand + 3] || patch->trigger || patch->grip || Any(patch->stick) ||
									   patch->primary || patch->secondary;
			}
	}
	if (!m_timed)
	{
		std::stable_sort(m_steps.begin(), m_steps.end(),
						 [](const auto& left, const auto& right) { return left.frame < right.frame; });
		return;
	}
	for (const auto& step : m_steps)
		if (step.time < Duration{} || step.time > MaxTime)
			throw std::invalid_argument("Pose sequence time is out of range");
	std::stable_sort(m_steps.begin(), m_steps.end(),
					 [](const auto& left, const auto& right) { return left.time < right.time; });
	for (const auto& step : m_steps)
	{
		for (std::size_t track = 0; track < m_scalars.size(); ++track)
			if (const auto* value = TrackSlot(step, track); value && *value)
				m_scalars[track].push_back({step.time, **value});
		for (std::size_t hand = 0; hand < m_orientations.size(); ++hand)
			if (step.controllers[hand] && step.controllers[hand]->orientation)
				m_orientations[hand].push_back({step.time, *step.controllers[hand]->orientation});
	}
}

std::size_t PoseSequence::Size() const noexcept
{
	return m_steps.size();
}

std::span<const PoseSequenceStep> PoseSequence::Steps() const noexcept
{
	return m_steps;
}

bool PoseSequence::Loops() const noexcept
{
	return m_loop;
}

const std::array<bool, 5>& PoseSequence::Channels() const noexcept
{
	return m_channels;
}

bool PoseSequence::Timed() const noexcept
{
	return m_timed;
}

Duration PoseSequence::Length() const noexcept
{
	return m_timed && !m_steps.empty() ? m_steps.back().time : Duration{};
}

PoseSequenceSample PoseSequence::Sample(Duration elapsed, const PoseSequenceTrackValues& start) const
{
	PoseSequenceSample sample;
	for (std::size_t track = 0; track < m_scalars.size(); ++track)
		if (!m_scalars[track].empty())
			*TrackSlot(sample, track) =
				Interpolate(m_scalars[track], elapsed, start.scalars[track], [](float from, float to, double fraction) {
					return static_cast<float>(from + (to - from) * fraction);
				});
	for (std::size_t hand = 0; hand < m_orientations.size(); ++hand)
		if (!m_orientations[hand].empty())
			sample.controllers[hand].orientation =
				Interpolate(m_orientations[hand], elapsed, start.orientations[hand], Slerp);
	return sample;
}

PoseSequenceTrackValues PoseSequence::End(const PoseSequenceTrackValues& start) const
{
	auto end = start;
	for (std::size_t track = 0; track < m_scalars.size(); ++track)
		if (!m_scalars[track].empty())
			end.scalars[track] = m_scalars[track].back().value;
	for (std::size_t hand = 0; hand < m_orientations.size(); ++hand)
		if (!m_orientations[hand].empty())
			end.orientations[hand] = m_orientations[hand].back().value;
	return end;
}

void PoseSequencePlayback::Start(PoseSequence sequence, FrameId origin)
{
	Start(std::make_shared<const PoseSequence>(std::move(sequence)), origin);
}

void PoseSequencePlayback::Start(std::shared_ptr<const PoseSequence> sequence, FrameId origin)
{
	if (sequence && sequence->Size() &&
		sequence->Steps().back().frame.value > std::numeric_limits<std::uint64_t>::max() - origin.value)
		throw std::overflow_error("Pose sequence extends beyond the frame counter");
	m_sequence = std::move(sequence);
	m_origin = origin;
	m_next = 0;
}

std::span<const PoseSequenceStep> PoseSequencePlayback::Advance(FrameId submitted) noexcept
{
	const auto steps = m_sequence ? m_sequence->Steps() : std::span<const PoseSequenceStep>{};
	if (steps.empty() || submitted < m_origin)
		return {};
	auto elapsed = submitted.value - m_origin.value;
	if (m_sequence->Loops() && elapsed > steps.back().frame.value)
	{
		// The strict comparison proves last+1 fits even at the uint64 boundary.
		elapsed %= steps.back().frame.value + 1;
		m_origin = {submitted.value - elapsed};
		m_next = 0;
	}
	const auto first = m_next;
	while (m_next < steps.size() && steps[m_next].frame.value <= elapsed)
		++m_next;
	return steps.subspan(first, m_next - first);
}

std::optional<FrameId> PoseSequencePlayback::NextFrame() const noexcept
{
	const auto steps = m_sequence ? m_sequence->Steps() : std::span<const PoseSequenceStep>{};
	if (steps.empty())
		return {};
	std::uint64_t offset = 0;
	if (m_next < steps.size())
		offset = steps[m_next].frame.value;
	else
	{
		if (!m_sequence->Loops() || steps.back().frame.value == std::numeric_limits<std::uint64_t>::max())
			return {};
		const auto cycle = steps.back().frame.value + 1;
		if (steps.front().frame.value > std::numeric_limits<std::uint64_t>::max() - cycle)
			return {};
		offset = cycle + steps.front().frame.value;
	}
	if (offset > std::numeric_limits<std::uint64_t>::max() - m_origin.value)
		return {};
	return FrameId{m_origin.value + offset};
}

bool PoseSequencePlayback::Complete() const noexcept
{
	return !m_sequence || !m_sequence->Size() || (!m_sequence->Loops() && m_next == m_sequence->Size());
}
} // namespace simxr
