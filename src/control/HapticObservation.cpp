#include "control/HapticObservation.h"
#include "Pch.h"
#include "control/ControlSchema.h"

namespace simxr::control
{
Field EncodeHaptics(std::span<const HapticOutput> outputs)
{
	outputs = outputs.last(std::min(outputs.size(), MaxObservedHaptics));
	std::vector<Field> events;
	for (const auto& output : outputs)
	{
		const std::vector fields{Field::U64(1, output.sequence),
								 Field::U32(2, output.device.kind == InputDeviceKind::Controller ? 0 : 1),
								 Field::U64(3, output.device.value),
								 Field::F32(4, output.amplitude),
								 Field::F32(5, output.frequency),
								 Field::I64(6, output.start.value.count()),
								 Field::I64(7, output.end ? output.end->value.count() : -1),
								 Field::U32(8, static_cast<std::uint32_t>(output.status))};
		events.push_back(Field::Bytes(static_cast<std::uint16_t>(events.size() + 1), EncodeFields(fields)));
	}
	return Field::Bytes(tag::Haptics, EncodeFields(events));
}
static const Field& Required(const Message& message, std::uint16_t tag)
{
	const auto* field = message.Find(tag);
	if (!field)
		throw std::invalid_argument("Missing haptic observation field");
	return *field;
}
HapticObservation DecodeHaptics(const Message& message)
{
	HapticObservation observation;
	const auto time = Required(message, tag::Time).AsI64();
	const auto overflow = Required(message, tag::Overflow).AsBool();
	if (!time || *time < 0 || !overflow)
		throw std::invalid_argument("Invalid haptic observation time");
	observation.time = *time;
	observation.overflow = *overflow;
	const auto list = DecodeFields(Required(message, tag::Haptics).value);
	if (list.error != DecodeError::None)
		throw std::invalid_argument("Invalid haptic observation list");
	for (const auto& entry : list.fields)
	{
		auto fields = DecodeFields(entry.value);
		if (fields.error != DecodeError::None || entry.tag != observation.events.size() + 1)
			throw std::invalid_argument("Invalid haptic observation event");
		Message event;
		event.fields = std::move(fields.fields);
		const auto sequence = Required(event, 1).AsU64();
		const auto kind = Required(event, 2).AsU32();
		const auto device = Required(event, 3).AsU64();
		const auto amplitude = Required(event, 4).AsF32();
		const auto frequency = Required(event, 5).AsF32();
		const auto start = Required(event, 6).AsI64();
		const auto end = Required(event, 7).AsI64();
		const auto status = Required(event, 8).AsU32();
		if (!sequence || !*sequence ||
			(!observation.events.empty() && *sequence <= observation.events.back().sequence) || !kind || !device ||
			!((*kind == 0 && *device < 2) || (*kind == 1 && *device != 0)) || !amplitude ||
			!std::isfinite(*amplitude) || *amplitude < 0 || *amplitude > 1 || !frequency ||
			!std::isfinite(*frequency) || *frequency < 0 || !start || *start < 0 || !end ||
			(*end != -1 && *end < *start) || !status || *status > static_cast<std::uint32_t>(HapticStatus::FocusLost))
			throw std::invalid_argument("Invalid haptic observation parameters");
		observation.events.push_back({*sequence,
									  {*kind == 0 ? InputDeviceKind::Controller : InputDeviceKind::Tracker, *device},
									  *amplitude,
									  *frequency,
									  *start,
									  *end == -1 ? std::nullopt : std::optional{*end},
									  static_cast<HapticStatus>(*status)});
	}
	return observation;
}
} // namespace simxr::control
