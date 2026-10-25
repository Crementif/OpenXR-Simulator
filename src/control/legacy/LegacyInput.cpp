#include "control/legacy/LegacyInput.h"
#include "Pch.h"

#include "math/Math.h"
#include "json.h"

#include <charconv>
#include <cmath>
#include <istream>
#include <limits>
#include <stdexcept>

namespace simxr
{
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

static void ApplyPosition(XrVector3f& position, const std::array<std::optional<float>, 3>& update)
{
	position.x = update[0].value_or(position.x);
	position.y = update[1].value_or(position.y);
	position.z = update[2].value_or(position.z);
}

LegacyInput::LegacyInput()
{
	m_controllers[0].offset = {{0, 0, 0, 1}, {-0.2f, -0.3f, -0.4f}};
	m_controllers[1].offset = {{0, 0, 0, 1}, {0.2f, -0.3f, -0.4f}};
	m_views.outerWidth = m_views.outerHeight = 1280;
	m_views.outerFov.fill({-0.7853982f, 0.7853982f, 0.7853982f, -0.7853982f});
}

LegacyInput::HeadUpdate LegacyInput::DecodeHead(const json::Object& document)
{
	return {{ReadNumber(document, "x"), ReadNumber(document, "y"), ReadNumber(document, "z")},
			{ReadNumber(document, "yaw"), ReadNumber(document, "pitch"), ReadNumber(document, "roll")}};
}

LegacyInput::ControllerUpdate LegacyInput::DecodeController(const json::Object& document)
{
	ControllerUpdate update;
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

void LegacyInput::ApplyHead(const HeadUpdate& update)
{
	ApplyPosition(m_head.position, update.position);
	m_headAngles.yaw = update.angles[0].value_or(m_headAngles.yaw);
	m_headAngles.pitch = update.angles[1].value_or(m_headAngles.pitch);
	m_headAngles.roll = update.angles[2].value_or(m_headAngles.roll);
	m_head.orientation = EulerAngles(m_headAngles.yaw, m_headAngles.pitch, m_headAngles.roll);
}

void LegacyInput::ApplyController(std::size_t hand, const ControllerUpdate& update)
{
	auto& controller = m_controllers[hand];
	ApplyPosition(controller.offset.position, update.position);
	if (update.orientation)
		controller.offset.orientation = *update.orientation;
	auto& controls = controller.controls;
	controls.trigger = std::clamp(update.trigger.value_or(controls.trigger), 0.0f, 1.0f);
	controls.grip = std::clamp(update.grip.value_or(controls.grip), 0.0f, 1.0f);
	controls.stick.x = std::clamp(update.stick[0].value_or(controls.stick.x), -1.0f, 1.0f);
	controls.stick.y = std::clamp(update.stick[1].value_or(controls.stick.y), -1.0f, 1.0f);
	controls.primary = update.primary.value_or(controls.primary);
	controls.secondary = update.secondary.value_or(controls.secondary);
}

void LegacyInput::ApplyControllerHold(const json::Object& document)
{
	const int selectedHand = static_cast<int>(std::clamp(NumberOr(document, "hand", -1), -1.0f, 2.0f));
	for (std::size_t hand = 0; hand < m_controllers.size(); ++hand)
	{
		if (selectedHand >= 0 && selectedHand != static_cast<int>(hand))
			continue;
		auto& controls = m_controllers[hand].controls;
		if (document.Boolean("clear", NumberOr(document, "clear", 0) != 0))
			controls = {};
		ControllerUpdate update;
		update.trigger = ReadNumber(document, "trigger");
		update.grip = ReadNumber(document, "grip");
		update.stick = {ReadNumber(document, "stickX"), ReadNumber(document, "stickY")};
		const auto heldButton = [&](const char* name, bool previous)
		{
			return document.Boolean(name, NumberOr(document, name, previous ? 1.0f : 0.0f) >= 0.5f);
		};
		update.primary = heldButton("primary", controls.primary);
		update.secondary = heldButton("secondary", controls.secondary);
		ApplyController(hand, update);
		controls.menu = heldButton("menu", controls.menu);
		controls.thumbClick = heldButton("thumbClick", controls.thumbClick);
	}
}

void LegacyInput::Apply(LegacyInputCommand command, const std::string& text)
{
	const json::Object document(text.c_str());
	if (!document.Valid())
		return;
	switch (command)
	{
	case LegacyInputCommand::HeadPose:
	{
		auto update = DecodeHead(document);
		update.position = {NumberOr(document, "x", 0), NumberOr(document, "y", 1.7f), NumberOr(document, "z", 0)};
		update.angles[0] = NumberOr(document, "yaw", 0);
		update.angles[1] = NumberOr(document, "pitch", 0);
		ApplyHead(update);
		break;
	}
	case LegacyInputCommand::ControllerPose:
	{
		const auto hand = static_cast<std::size_t>(std::clamp(NumberOr(document, "hand", 1), 0.0f, 1.0f));
		ControllerUpdate update;
		update.position = {NumberOr(document, "posX", hand ? 0.2f : -0.2f), NumberOr(document, "posY", -0.3f),
						   NumberOr(document, "posZ", -0.4f)};
		update.orientation = EulerAngles(NumberOr(document, "yaw", 0), NumberOr(document, "pitch", -0.3f),
										 NumberOr(document, "roll", 0));
		update.trigger = ReadNumber(document, "trigger");
		update.primary = ReadButton(document, "buttonA");
		ApplyController(hand, update);
		break;
	}
	case LegacyInputCommand::ControllerHold:
		ApplyControllerHold(document);
		break;
	case LegacyInputCommand::Ipd:
		m_views.ipd =
			document.Boolean("clear", false)
				? 0.064f
				: std::clamp(NumberOr(document, "ipd_mm", NumberOr(document, "ipd", 0.064f) * 1000) / 1000, 0.0f, 0.2f);
		break;
	case LegacyInputCommand::Fov:
		for (std::size_t eye = 0; eye < m_views.outerFov.size(); ++eye)
		{
			auto source = document.Child(eye ? "right" : "left");
			if (!source.Valid())
				source = document;
			auto& fov = m_views.outerFov[eye];
			fov = {NumberOr(source, "aL", NumberOr(source, "angleLeft", fov.angleLeft)),
				   NumberOr(source, "aR", NumberOr(source, "angleRight", fov.angleRight)),
				   NumberOr(source, "aU", NumberOr(source, "angleUp", fov.angleUp)),
				   NumberOr(source, "aD", NumberOr(source, "angleDown", fov.angleDown))};
		}
		break;
	}
}

LegacyInput::Sequence LegacyInput::ReadSequence(std::istream& input, bool loop)
{
	std::vector<PoseSequenceStep> sequence;
	std::string line;
	while (std::getline(input, line))
	{
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
	if (input.bad() || (input.fail() && !input.eof()))
		throw std::runtime_error("Cannot read complete pose sequence");
	return {std::move(sequence), loop};
}

void LegacyInput::StartSequence(Sequence sequence, FrameId origin)
{
	m_sequence.Start(std::move(sequence), origin);
}

bool LegacyInput::AdvanceSequence(FrameId submitted)
{
	bool capture = false;
	for (const auto& step : m_sequence.Advance(submitted))
	{
		if (step.head)
			ApplyHead(*step.head);
		for (std::size_t hand = 0; hand < step.controllers.size(); ++hand)
			if (step.controllers[hand])
				ApplyController(hand, *step.controllers[hand]);
		capture |= step.capture;
	}
	return capture;
}

TrackingState LegacyInput::Tracking(bool focused) const
{
	TrackingState tracking;
	tracking.head.pose = m_head;
	tracking.focused = focused;
	for (std::size_t hand = 0; hand < m_controllers.size(); ++hand)
		tracking.controllers[hand].pose.pose = Compose(m_head, m_controllers[hand].offset);
	return tracking;
}

std::array<ControllerControls, 2> LegacyInput::Controls() const
{
	std::array<ControllerControls, 2> result;
	for (std::size_t hand = 0; hand < result.size(); ++hand)
	{
		auto& controls = result[hand];
		controls = m_controllers[hand].controls;
		controls.gripForce = controls.grip;
		controls.trackpad = controls.stick;
		controls.primaryTouch = controls.primary;
		controls.secondaryTouch = controls.secondary;
		controls.trackpadClick = controls.thumbClick;
		controls.triggerTouch = controls.trigger > 0;
		controls.thumbTouch = controls.trackpadTouch = controls.thumbClick;
	}
	return result;
}

const XrPosef& LegacyInput::HeadPose() const
{
	return m_head;
}

LegacyHeadAngles LegacyInput::HeadAngles() const
{
	return m_headAngles;
}

const ViewSettings& LegacyInput::Views() const
{
	return m_views;
}

void LegacyInput::SetViews(ViewSettings settings)
{
	m_views = std::move(settings);
}
} // namespace simxr
