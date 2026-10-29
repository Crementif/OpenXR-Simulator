#include "control/WireProtocol.h"
#include "Pch.h"

namespace simxr::control
{
static constexpr std::size_t s_fieldHeaderSize = 8;

template <typename T> static T Read(std::span<const std::byte> bytes, std::size_t offset = 0) noexcept
{
	std::uint64_t result = 0;
	for (std::size_t index = 0; index < sizeof(T); ++index)
		result |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + index])} << (index * 8);
	return static_cast<T>(result);
}

template <typename T> static void Write(std::span<std::byte> bytes, std::size_t offset, T value) noexcept
{
	for (std::size_t index = 0; index < sizeof(T); ++index)
		bytes[offset + index] = std::byte{static_cast<unsigned char>(value >> (index * 8))};
}

static bool ValidText(std::span<const std::byte> bytes) noexcept
{
	std::size_t offset = 0;
	while (offset < bytes.size())
	{
		const auto first = std::to_integer<unsigned char>(bytes[offset++]);
		if (!first)
			return false;
		if (first < 0x80)
			continue;
		unsigned remaining;
		unsigned minimumSecond = 0x80, maximumSecond = 0xbf;
		if (first >= 0xc2 && first <= 0xdf)
			remaining = 1;
		else if (first >= 0xe0 && first <= 0xef)
		{
			remaining = 2;
			if (first == 0xe0)
				minimumSecond = 0xa0;
			if (first == 0xed)
				maximumSecond = 0x9f;
		}
		else if (first >= 0xf0 && first <= 0xf4)
		{
			remaining = 3;
			if (first == 0xf0)
				minimumSecond = 0x90;
			if (first == 0xf4)
				maximumSecond = 0x8f;
		}
		else
			return false;
		if (bytes.size() - offset < remaining)
			return false;
		const auto second = std::to_integer<unsigned char>(bytes[offset++]);
		if (second < minimumSecond || second > maximumSecond)
			return false;
		for (unsigned index = 1; index < remaining; ++index)
		{
			const auto next = std::to_integer<unsigned char>(bytes[offset++]);
			if (next < 0x80 || next > 0xbf)
				return false;
		}
	}
	return true;
}

static DecodeError CheckFields(std::span<const std::byte> payload, std::size_t& count) noexcept
{
	if (payload.size() > MaxPayload)
		return DecodeError::PayloadTooLarge;
	std::array<std::uint16_t, MaxFields> tags{};
	std::size_t offset = 0;
	count = 0;
	while (offset < payload.size())
	{
		if (count == MaxFields)
			return DecodeError::TooManyFields;
		if (payload.size() - offset < s_fieldHeaderSize)
			return DecodeError::InvalidFieldLength;
		const auto tag = Read<std::uint16_t>(payload, offset);
		const auto flags = Read<std::uint16_t>(payload, offset + 2);
		const auto length = Read<std::uint32_t>(payload, offset + 4);
		if (!tag)
			return DecodeError::InvalidTag;
		if (flags > 1)
			return DecodeError::InvalidFieldFlags;
		if (std::find(tags.begin(), tags.begin() + count, tag) != tags.begin() + count)
			return DecodeError::DuplicateTag;
		tags[count++] = tag;
		offset += s_fieldHeaderSize;
		if (length > payload.size() - offset)
			return DecodeError::InvalidFieldLength;
		offset += length;
	}
	return DecodeError::None;
}

template <typename T> static Field Integer(std::uint16_t tag, T value, bool required)
{
	Field result{tag, required, std::vector<std::byte>(sizeof(T))};
	Write<T>(result.value, 0, value);
	return result;
}

Field Field::U64(std::uint16_t tag, std::uint64_t value, bool required)
{
	return Integer(tag, value, required);
}
Field Field::U32(std::uint16_t tag, std::uint32_t value, bool required)
{
	return Integer(tag, value, required);
}
Field Field::I64(std::uint16_t tag, std::int64_t value, bool required)
{
	return Integer(tag, std::bit_cast<std::uint64_t>(value), required);
}
Field Field::F32(std::uint16_t tag, float value, bool required)
{
	static_assert(sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559);
	return Integer(tag, std::bit_cast<std::uint32_t>(value), required);
}
Field Field::Bool(std::uint16_t tag, bool value, bool required)
{
	return Field{tag, required, {std::byte{static_cast<unsigned char>(value)}}};
}
Field Field::Text(std::uint16_t tag, std::string_view value, bool required)
{
	const auto bytes = std::as_bytes(std::span{value.data(), value.size()});
	if (!ValidText(bytes))
		throw std::invalid_argument("Control text must be NUL-free UTF-8");
	return Bytes(tag, bytes, required);
}
Field Field::Bytes(std::uint16_t tag, std::span<const std::byte> value, bool required)
{
	if (value.size() > MaxPayload - s_fieldHeaderSize)
		throw std::length_error("Control field exceeds payload bound");
	return Field{tag, required, {value.begin(), value.end()}};
}
std::optional<std::uint64_t> FieldView::AsU64() const noexcept
{
	return value.size() == sizeof(std::uint64_t) ? std::optional{Read<std::uint64_t>(value)} : std::nullopt;
}
std::optional<std::uint32_t> FieldView::AsU32() const noexcept
{
	return value.size() == sizeof(std::uint32_t) ? std::optional{Read<std::uint32_t>(value)} : std::nullopt;
}
std::optional<std::int64_t> FieldView::AsI64() const noexcept
{
	const auto integer = AsU64();
	return integer ? std::optional{std::bit_cast<std::int64_t>(*integer)} : std::nullopt;
}
std::optional<float> FieldView::AsF32() const noexcept
{
	const auto integer = AsU32();
	return integer ? std::optional{std::bit_cast<float>(*integer)} : std::nullopt;
}
std::optional<bool> FieldView::AsBool() const noexcept
{
	return value.size() == 1 && std::to_integer<unsigned char>(value.front()) <= 1
			   ? std::optional{value.front() == std::byte{1}}
			   : std::nullopt;
}
std::optional<std::string_view> FieldView::AsText() const noexcept
{
	if (!ValidText(value))
		return {};
	return value.empty() ? std::string_view{}
						 : std::string_view{reinterpret_cast<const char*>(value.data()), value.size()};
}
std::optional<std::uint64_t> Field::AsU64() const noexcept
{
	return FieldView{tag, required, value}.AsU64();
}
std::optional<std::uint32_t> Field::AsU32() const noexcept
{
	return FieldView{tag, required, value}.AsU32();
}
std::optional<std::int64_t> Field::AsI64() const noexcept
{
	return FieldView{tag, required, value}.AsI64();
}
std::optional<float> Field::AsF32() const noexcept
{
	return FieldView{tag, required, value}.AsF32();
}
std::optional<bool> Field::AsBool() const noexcept
{
	return FieldView{tag, required, value}.AsBool();
}
std::optional<std::string_view> Field::AsText() const noexcept
{
	return FieldView{tag, required, value}.AsText();
}
const FieldView* FieldViewsResult::Find(std::uint16_t tag) const noexcept
{
	for (const auto& field : fields)
		if (field.tag == tag)
			return &field;
	return nullptr;
}
Field* Message::Find(std::uint16_t tag) noexcept
{
	return const_cast<Field*>(std::as_const(*this).Find(tag));
}
const Field* Message::Find(std::uint16_t tag) const noexcept
{
	const auto field = std::find_if(fields.begin(), fields.end(),
									[&](const auto& value)
									{
										return value.tag == tag;
									});
	return field == fields.end() ? nullptr : &*field;
}
std::optional<std::uint16_t> Message::ValidateFields(std::span<const std::uint16_t> knownTags) const noexcept
{
	for (const auto& field : fields)
		if (field.required && std::find(knownTags.begin(), knownTags.end(), field.tag) == knownTags.end())
			return field.tag;
	return {};
}

std::vector<std::byte> EncodeFields(std::span<const Field> fields)
{
	if (fields.size() > MaxFields)
		throw std::length_error("Too many control fields");
	std::array<std::uint16_t, MaxFields> tags{};
	std::size_t count = 0, size = 0;
	for (const auto& field : fields)
	{
		if (!field.tag || std::find(tags.begin(), tags.begin() + count, field.tag) != tags.begin() + count)
			throw std::invalid_argument("Control fields need distinct nonzero tags");
		tags[count++] = field.tag;
		if (field.value.size() > MaxPayload - s_fieldHeaderSize ||
			size > MaxPayload - s_fieldHeaderSize - field.value.size())
			throw std::length_error("Control payload exceeds bound");
		size += s_fieldHeaderSize + field.value.size();
	}
	std::vector<std::byte> result(size);
	std::size_t offset = 0;
	for (const auto& field : fields)
	{
		Write<std::uint16_t>(result, offset, field.tag);
		Write<std::uint16_t>(result, offset + 2, field.required ? 1 : 0);
		Write<std::uint32_t>(result, offset + 4, static_cast<std::uint32_t>(field.value.size()));
		offset += s_fieldHeaderSize;
		if (!field.value.empty())
			std::memcpy(result.data() + offset, field.value.data(), field.value.size());
		offset += field.value.size();
	}
	return result;
}

std::vector<std::byte> Encode(const Message& message)
{
	if (message.major != ProtocolMajor || message.flags > 1 || !message.requestId.value)
		throw std::invalid_argument("Invalid outgoing control envelope");
	auto payload = EncodeFields(message.fields);
	std::vector<std::byte> result(HeaderSize + payload.size());
	Write<std::uint32_t>(result, 0, Magic);
	Write<std::uint16_t>(result, 4, message.major);
	Write<std::uint16_t>(result, 6, message.minor);
	Write<std::uint16_t>(result, 8, message.opcode);
	Write<std::uint16_t>(result, 10, message.flags);
	Write<std::uint64_t>(result, 16, message.requestId.value);
	Write<std::uint32_t>(result, 24, static_cast<std::uint32_t>(payload.size()));
	if (!payload.empty())
		std::memcpy(result.data() + HeaderSize, payload.data(), payload.size());
	return result;
}

template <class Result> static Result DecodeFieldStorage(std::span<const std::byte> payload) noexcept
{
	std::size_t count = 0;
	const auto error = CheckFields(payload, count);
	if (error != DecodeError::None)
		return {{}, error};
	try
	{
		typename decltype(Result{}.fields)::value_type field;
		decltype(Result{}.fields) result;
		result.reserve(count);
		std::size_t offset = 0;
		while (offset < payload.size())
		{
			const auto tag = Read<std::uint16_t>(payload, offset);
			const auto required = Read<std::uint16_t>(payload, offset + 2) != 0;
			const auto length = Read<std::uint32_t>(payload, offset + 4);
			offset += s_fieldHeaderSize;
			const auto value = payload.subspan(offset, length);
			field.tag = tag;
			field.required = required;
			if constexpr (std::is_same_v<Result, FieldViewsResult>)
				field.value = value;
			else
				field.value.assign(value.begin(), value.end());
			result.push_back(std::move(field));
			offset += length;
		}
		return {std::move(result), DecodeError::None};
	}
	catch (...)
	{
		return {{}, DecodeError::AllocationFailure};
	}
}

FieldsResult DecodeFields(std::span<const std::byte> payload) noexcept
{
	return DecodeFieldStorage<FieldsResult>(payload);
}
FieldViewsResult DecodeFieldViews(std::span<const std::byte> payload) noexcept
{
	return DecodeFieldStorage<FieldViewsResult>(payload);
}

DecodeResult Decode(std::span<const std::byte> bytes) noexcept
{
	if (bytes.size() < HeaderSize)
		return {{}, DecodeError::Incomplete, HeaderSize};
	if (Read<std::uint32_t>(bytes) != Magic)
		return {{}, DecodeError::InvalidMagic};
	if (Read<std::uint16_t>(bytes, 4) != ProtocolMajor)
		return {{}, DecodeError::UnsupportedVersion};
	if (Read<std::uint16_t>(bytes, 10) > 1 || Read<std::uint32_t>(bytes, 12) || Read<std::uint32_t>(bytes, 28))
		return {{}, DecodeError::InvalidHeader};
	if (!Read<std::uint64_t>(bytes, 16))
		return {{}, DecodeError::InvalidRequestId};
	const auto payloadSize = Read<std::uint32_t>(bytes, 24);
	if (payloadSize > MaxPayload)
		return {{}, DecodeError::PayloadTooLarge};
	const auto size = HeaderSize + payloadSize;
	if (bytes.size() < size)
		return {{}, DecodeError::Incomplete, size};
	if (bytes.size() != size)
		return {{}, DecodeError::TrailingBytes};
	auto fields = DecodeFields(bytes.subspan(HeaderSize));
	if (fields.error != DecodeError::None)
		return {{}, fields.error};
	Message message{Read<std::uint16_t>(bytes, 4),
					Read<std::uint16_t>(bytes, 6),
					Read<std::uint16_t>(bytes, 8),
					Read<std::uint16_t>(bytes, 10),
					RequestId{Read<std::uint64_t>(bytes, 16)},
					std::move(fields.fields)};
	return {std::move(message), DecodeError::None, size};
}
} // namespace simxr::control
