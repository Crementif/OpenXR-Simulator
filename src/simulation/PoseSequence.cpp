#include "simulation/PoseSequence.h"
#include "Pch.h"
#include "json.h"
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
	std::string line;
	while (!source.empty())
	{
		const auto end = source.find('\n');
		line.assign(source.substr(0, end));
		source.remove_prefix(end == std::string_view::npos ? source.size() : end + 1);
		const json::Object document(line.c_str());
		if (!document.Valid())
			continue;
		const auto frame = SequenceFrame(document);
		if (!frame)
			continue;
		if (sequence.size() == PoseSequence::MaxSteps)
			throw std::length_error("Pose sequence exceeds its step limit");
		PoseSequenceStep step;
		step.frame = {*frame};
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
	return {std::move(sequence), loop};
}

PoseSequence::PoseSequence(std::vector<PoseSequenceStep> steps, bool loop) : m_steps(std::move(steps)), m_loop(loop)
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
	std::stable_sort(m_steps.begin(), m_steps.end(),
					 [](const auto& left, const auto& right) { return left.frame < right.frame; });
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
