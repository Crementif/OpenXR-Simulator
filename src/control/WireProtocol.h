#pragma once

#include "control/ProtocolTypes.h"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace simxr::control
{
// Borrows the encoded payload; its owner must outlive this view.
struct FieldView
{
	std::uint16_t tag = 0;
	bool required = false;
	std::span<const std::byte> value;
	std::optional<std::uint64_t> AsU64() const noexcept;
	std::optional<std::uint32_t> AsU32() const noexcept;
	std::optional<std::int64_t> AsI64() const noexcept;
	std::optional<float> AsF32() const noexcept;
	std::optional<bool> AsBool() const noexcept;
	// The returned view borrows this field's storage.
	std::optional<std::string_view> AsText() const noexcept;
};

struct Field
{
	std::uint16_t tag = 0;
	bool required = false;
	std::vector<std::byte> value;

	static Field U64(std::uint16_t tag, std::uint64_t value, bool required = false);
	static Field U32(std::uint16_t tag, std::uint32_t value, bool required = false);
	static Field I64(std::uint16_t tag, std::int64_t value, bool required = false);
	static Field F32(std::uint16_t tag, float value, bool required = false);
	static Field Bool(std::uint16_t tag, bool value, bool required = false);
	// Text is NUL-free, well-formed UTF-8; invalid outgoing text throws invalid_argument.
	static Field Text(std::uint16_t tag, std::string_view value, bool required = false);
	static Field Bytes(std::uint16_t tag, std::span<const std::byte> value, bool required = false);
	std::optional<std::uint64_t> AsU64() const noexcept;
	std::optional<std::uint32_t> AsU32() const noexcept;
	std::optional<std::int64_t> AsI64() const noexcept;
	std::optional<float> AsF32() const noexcept;
	std::optional<bool> AsBool() const noexcept;
	// The returned view borrows this field's storage.
	std::optional<std::string_view> AsText() const noexcept;
};

struct Message
{
	std::uint16_t major = ProtocolMajor;
	std::uint16_t minor = ProtocolMinor;
	// Retain unknown commands for explicit UnsupportedCommand service responses.
	std::uint16_t opcode = 0;
	std::uint16_t flags = 0;
	RequestId requestId;
	std::vector<Field> fields;

	Field* Find(std::uint16_t tag) noexcept;
	const Field* Find(std::uint16_t tag) const noexcept;
	// nullopt accepts all fields; otherwise identifies the first unknown critical tag.
	// Optional unknown fields remain in the decoded message and may be ignored.
	std::optional<std::uint16_t> ValidateFields(std::span<const std::uint16_t> knownTags) const noexcept;
};

enum class DecodeError
{
	None,
	Incomplete,
	InvalidMagic,
	UnsupportedVersion,
	InvalidHeader,
	InvalidRequestId,
	PayloadTooLarge,
	TrailingBytes,
	TooManyFields,
	InvalidTag,
	InvalidFieldFlags,
	DuplicateTag,
	InvalidFieldLength,
	AllocationFailure
};

struct FieldsResult
{
	std::vector<Field> fields;
	DecodeError error = DecodeError::None;
};

struct FieldViewsResult
{
	std::vector<FieldView> fields;
	DecodeError error = DecodeError::None;
	const FieldView* Find(std::uint16_t tag) const noexcept;
};

struct DecodeResult
{
	std::optional<Message> message;
	DecodeError error = DecodeError::None;
	// For Incomplete, buffer exactly this many total bytes before retrying.
	// For success, equals the exact encoded message size. Zero for other errors.
	std::size_t requiredBytes = 0;
};

// Outgoing invalid metadata throws invalid_argument; bounded-size violations
// throw length_error. Allocation failure propagates to the service boundary.
std::vector<std::byte> EncodeFields(std::span<const Field> fields);
std::vector<std::byte> Encode(const Message& message);
// Nested fields must be complete; truncated TLVs report InvalidFieldLength.
FieldsResult DecodeFields(std::span<const std::byte> payload) noexcept;
FieldViewsResult DecodeFieldViews(std::span<const std::byte> payload) noexcept;
// Exact framing: reports Incomplete for stream prefixes and rejects trailing bytes.
// No partially decoded message/fields are exposed on malformed input.
DecodeResult Decode(std::span<const std::byte> bytes) noexcept;
} // namespace simxr::control
