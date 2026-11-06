#include "../TestSupport.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"
#include "control/WireProtocol.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace
{
using namespace simxr::control;

void Put32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
{
	for (unsigned index = 0; index < 4; ++index)
		bytes.at(offset + index) = std::byte{static_cast<unsigned char>(value >> (index * 8))};
}

void RequireError(std::span<const std::byte> bytes, DecodeError expected, const char* message)
{
	const auto decoded = Decode(bytes);
	Require(!decoded.message && decoded.error == expected && decoded.requiredBytes == 0, message);
}

Message Request()
{
	Message result;
	result.opcode = static_cast<std::uint16_t>(OpCode::Submit);
	result.requestId = RequestId{0xfedcba9876543210ULL};
	return result;
}

template<typename Exception, typename Function>
void RequireThrows(Function&& function, const char* message)
{
	bool threw = false;
	try
	{
		function();
	}
	catch (const Exception&)
	{
		threw = true;
	}
	Require(threw, message);
}

void GoldenEnvelope()
{
	auto message = Request();
	message.minor = 0; // Preserve the original v1.0 golden envelope.
	message.fields = {Field::U64(0x1234, 0x0123456789abcdefULL, true)};
	const std::array<unsigned char, 48> expected{0x53, 0x58, 0x52, 0x32, 1,	   0,	 0,	   0,	 7,	   0,	 0,	   0,
												 0,	   0,	 0,	   0,	 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
												 16,   0,	 0,	   0,	 0,	   0,	 0,	   0,	 0x34, 0x12, 1,	   0,
												 8,	   0,	 0,	   0,	 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 1};
	const auto encoded = Encode(message);
	Require(encoded.size() == expected.size() && std::memcmp(encoded.data(), expected.data(), expected.size()) == 0,
			"Golden little-endian envelope/TLV bytes changed");
	const auto decoded = Decode(encoded);
	Require(decoded.error == DecodeError::None && decoded.message && decoded.requiredBytes == expected.size() &&
				decoded.message->requestId == message.requestId && decoded.message->Find(0x1234)->required &&
				decoded.message->Find(0x1234)->AsU64() == 0x0123456789abcdefULL,
			"Golden envelope did not decode full-width IDs and required fields");
	static_assert(!std::is_convertible_v<RequestId, ControlSessionId>);
	static_assert(!std::is_convertible_v<ConnectionId, RequestId>);
}

void TypedAndNestedRecords()
{
	const std::string text = "\xe7\x9c\xbc gaze \xf0\x9f\x91\x81 \xc3\xa9";
	const std::array nested{Field::U64(1, std::numeric_limits<std::uint64_t>::max(), true), Field::Text(2, text)};
	auto message = Request();
	message.opcode = 0xbeef;
	message.minor = 42;
	message.flags = 1;
	message.requestId = RequestId{std::numeric_limits<std::uint64_t>::max()};
	message.fields = {Field::U64(1, std::numeric_limits<std::uint64_t>::max()),
					  Field::U32(2, std::numeric_limits<std::uint32_t>::max()),
					  Field::I64(3, std::numeric_limits<std::int64_t>::min()),
					  Field::I64(4, std::numeric_limits<std::int64_t>::max()),
					  Field::Bool(5, false),
					  Field::Bool(6, true),
					  Field::Text(7, text),
					  Field::Bytes(8, EncodeFields(nested)),
					  Field::Text(9, "")};
	auto payload = message.Find(8);
	const auto storage = payload->value.data();
	auto moved = std::move(payload->value);
	Require(moved.data() == storage && moved == EncodeFields(nested),
			"Taking owned field bytes copied or changed them");
	payload->value = std::move(moved);
	const auto encoded = Encode(message);
	const auto decoded = Decode(encoded);
	Require(decoded.message && decoded.message->minor == 42 && decoded.message->opcode == 0xbeef &&
				decoded.message->flags == 1 && decoded.message->requestId == message.requestId &&
				Encode(*decoded.message) == encoded,
			"Newer minor/unknown opcode or response was not preserved");
	const auto& result = *decoded.message;
	Require(result.Find(1)->AsU64() == std::numeric_limits<std::uint64_t>::max() &&
				result.Find(2)->AsU32() == std::numeric_limits<std::uint32_t>::max() &&
				result.Find(3)->AsI64() == std::numeric_limits<std::int64_t>::min() &&
				result.Find(4)->AsI64() == std::numeric_limits<std::int64_t>::max() &&
				result.Find(5)->AsBool() == false && result.Find(6)->AsBool() == true &&
				result.Find(7)->AsText() == text && result.Find(9)->AsText() == "" && !result.Find(10),
			"Typed values lost exact width/signedness/text/bool semantics");
	Require(!result.Find(1)->AsU32() && !result.Find(2)->AsU64() && !result.Find(7)->AsI64() &&
				!result.Find(1)->AsBool(),
			"Typed reader accepted wrong-width field");
	const auto child = DecodeFields(result.Find(8)->value);
	Require(child.error == DecodeError::None && child.fields.size() == 2 &&
				child.fields[0].AsU64() == std::numeric_limits<std::uint64_t>::max() &&
				child.fields[1].AsText() == text,
			"Nested TLV records did not round-trip independently");
	Require(DecodeFields({}).error == DecodeError::None, "Empty nested record is not valid");
}

void FloatRepresentations()
{
	const std::array<std::uint32_t, 8> bits{0,			 0x80000000U, 0x3f800000U, 0x00000001U,
											0x7f7fffffU, 0x7f800000U, 0xff800000U, 0x7fc01234U};
	for (const auto representation : bits)
	{
		const auto field = Field::F32(1, std::bit_cast<float>(representation));
		Require(field.value.size() == 4 && std::to_integer<unsigned>(field.value[0]) == (representation & 255) &&
					std::bit_cast<std::uint32_t>(*field.AsF32()) == representation,
				"Float32 encoding changed bits or endianness");
	}
	Require(!Field::U64(1, 1).AsF32() && !Field::Bytes(1, {}).AsF32(), "Float reader accepted wrong width");
}

void UnknownFields()
{
	auto message = Request();
	message.fields = {Field::U32(1, 7, true), Field::Text(60000, "future optional")};
	const auto decoded = Decode(Encode(message));
	const std::array<std::uint16_t, 1> known{1};
	Require(decoded.message && !decoded.message->ValidateFields(known) && decoded.message->Find(60000),
			"Unknown optional field was rejected or removed before service validation");
	message.fields.push_back(Field::U64(65000, 9, true));
	const auto critical = Decode(Encode(message));
	Require(critical.message && critical.message->ValidateFields(known) == 65000,
			"Unknown required field was not reported explicitly");
	const std::array<std::uint16_t, 2> extended{1, 65000};
	Require(!critical.message->ValidateFields(extended), "Known critical field was incorrectly rejected");
}

void IncrementalAndHeaderFailures()
{
	auto message = Request();
	message.fields = {Field::Text(1, "stream record", true)};
	const auto encoded = Encode(message);
	for (std::size_t size = 0; size < encoded.size(); ++size)
	{
		const auto prefix = Decode(std::span{encoded}.first(size));
		Require(!prefix.message && prefix.error == DecodeError::Incomplete &&
					prefix.requiredBytes == (size < HeaderSize ? HeaderSize : encoded.size()),
				"Incremental prefix was accepted or requested an incorrect frame bound");
	}
	auto bytes = encoded;
	bytes.push_back(std::byte{0});
	RequireError(bytes, DecodeError::TrailingBytes, "Trailing byte was silently consumed");
	bytes.insert(bytes.end(), encoded.begin(), encoded.end());
	RequireError(bytes, DecodeError::TrailingBytes, "Concatenated envelopes were accepted as one message");
	bytes = encoded;
	bytes[0] = std::byte{0};
	RequireError(bytes, DecodeError::InvalidMagic, "Bad magic accepted");
	bytes = encoded;
	bytes[4] = std::byte{2};
	RequireError(bytes, DecodeError::UnsupportedVersion, "Unsupported major was not explicit");
	for (const auto offset : {10U, 11U, 12U, 15U, 28U, 31U})
	{
		bytes = encoded;
		bytes[offset] = std::byte{2};
		RequireError(bytes, DecodeError::InvalidHeader, "Invalid flags/reserved bits accepted");
	}
	bytes = encoded;
	std::fill(bytes.begin() + 16, bytes.begin() + 24, std::byte{0});
	RequireError(bytes, DecodeError::InvalidRequestId, "Zero request ID accepted");
	bytes = encoded;
	Put32(bytes, 24, MaxPayload + 1);
	RequireError(bytes, DecodeError::PayloadTooLarge, "Oversize payload not rejected before buffering");
	Put32(bytes, 24, std::numeric_limits<std::uint32_t>::max());
	RequireError(bytes, DecodeError::PayloadTooLarge, "Overflow-sized payload accepted");
}

void InvalidFieldsAndLimits()
{
	auto message = Request();
	message.fields = {Field::U32(1, 1)};
	const auto encoded = Encode(message);
	auto bytes = encoded;
	bytes[HeaderSize] = std::byte{0};
	RequireError(bytes, DecodeError::InvalidTag, "Zero tag accepted");
	bytes = encoded;
	bytes[HeaderSize + 2] = std::byte{2};
	RequireError(bytes, DecodeError::InvalidFieldFlags, "Unknown TLV flag accepted");
	bytes = encoded;
	Put32(bytes, HeaderSize + 4, 5);
	RequireError(bytes, DecodeError::InvalidFieldLength, "TLV length crossed envelope payload");
	bytes = encoded;
	bytes.insert(bytes.end(), encoded.begin() + HeaderSize, encoded.end());
	Put32(bytes, 24, 24);
	RequireError(bytes, DecodeError::DuplicateTag, "Repeated tag accepted");
	const auto nested = DecodeFields(std::span{bytes}.subspan(HeaderSize));
	Require(nested.error == DecodeError::DuplicateTag && nested.fields.empty(),
			"Nested decode leaked partial fields on failure");
	for (unsigned length = 1; length < 8; ++length)
	{
		const auto shortHeader = DecodeFields(std::span{encoded}.subspan(HeaderSize, length));
		Require(shortHeader.error == DecodeError::InvalidFieldLength && shortHeader.fields.empty(),
				"Truncated nested header accepted");
	}
	message.fields.clear();
	for (std::uint16_t tag = 1; tag <= MaxFields; ++tag)
		message.fields.push_back(Field::Bytes(tag, {}));
	Require(Decode(Encode(message)).message->fields.size() == MaxFields, "Maximum field count rejected");
	bytes = Encode(message);
	const auto extra = EncodeFields(std::array{Field::Bytes(257, {})});
	bytes.insert(bytes.end(), extra.begin(), extra.end());
	Put32(bytes, 24, static_cast<std::uint32_t>(bytes.size() - HeaderSize));
	RequireError(bytes, DecodeError::TooManyFields, "More than 256 fields accepted");
	message.fields.push_back(Field::Bytes(257, {}));
	RequireThrows<std::length_error>([&] { Encode(message); }, "Outgoing field count was unbounded");
	message.fields = {Field::Bytes(1, std::vector<std::byte>(MaxPayload - 8))};
	const auto maximum = Encode(message);
	Require(maximum.size() == MaxPayload + HeaderSize && Decode(maximum).message,
			"Exact maximum payload boundary rejected");
	message.fields.push_back(Field::Bytes(2, {}));
	RequireThrows<std::length_error>([&] { Encode(message); }, "Outgoing aggregate payload bound bypassed");
	message = Request();
	message.fields = {Field::U32(1, 1), Field::U32(1, 2)};
	RequireThrows<std::invalid_argument>([&] { Encode(message); }, "Outgoing duplicate tags accepted");
	message.fields = {Field::U32(0, 1)};
	RequireThrows<std::invalid_argument>([&] { Encode(message); }, "Outgoing zero tag accepted");
	message.fields.clear();
	message.requestId = {};
	RequireThrows<std::invalid_argument>([&] { Encode(message); }, "Outgoing zero request ID accepted");
	message = Request();
	message.flags = 2;
	RequireThrows<std::invalid_argument>([&] { Encode(message); }, "Outgoing invalid flags accepted");
	message = Request();
	message.major = 2;
	RequireThrows<std::invalid_argument>([&] { Encode(message); }, "Outgoing unsupported major accepted");
}

void TextValidation()
{
	const std::array invalid{std::string{"a\0b", 3},
							 std::string{"\x80"},
							 std::string{"\xc0\x80"},
							 std::string{"\xc1\xbf"},
							 std::string{"\xc2"},
							 std::string{"\xc2\x7f"},
							 std::string{"\xe0\x80\x80"},
							 std::string{"\xed\xa0\x80"},
							 std::string{"\xef\xbf"},
							 std::string{"\xf0\x80\x80\x80"},
							 std::string{"\xf4\x90\x80\x80"},
							 std::string{"\xf5\x80\x80\x80"},
							 std::string{"\xf0\x90\x80"},
							 std::string{"\xff"}};
	for (const auto& text : invalid)
	{
		RequireThrows<std::invalid_argument>([&] { Field::Text(1, text); }, "Invalid outgoing UTF-8/NUL text accepted");
		const auto field = Field::Bytes(1, std::as_bytes(std::span{text.data(), text.size()}));
		Require(!field.AsText(), "Typed reader accepted malformed UTF-8/NUL text");
	}
	const std::array valid{std::string{},
						   std::string{"ASCII"},
						   std::string{"\xc2\x80"},
						   std::string{"\xdf\xbf"},
						   std::string{"\xe0\xa0\x80"},
						   std::string{"\xed\x9f\xbf"},
						   std::string{"\xee\x80\x80"},
						   std::string{"\xef\xbf\xbf"},
						   std::string{"\xf0\x90\x80\x80"},
						   std::string{"\xf4\x8f\xbf\xbf"}};
	for (const auto& text : valid)
		Require(Field::Text(1, text).AsText() == text, "Valid UTF-8 boundary rejected");
	Require(!Field::Bytes(1, std::array{std::byte{2}}).AsBool() && !Field::Bytes(1, {}).AsBool(),
			"Invalid boolean representation accepted");
}

void AdversarialLengths()
{
	auto message = Request();
	message.fields = {Field::U64(1, 0x123456789abcdef0ULL)};
	const auto encoded = Encode(message);
	std::mt19937 random(0x53585232);
	for (unsigned trial = 0; trial < 5000; ++trial)
	{
		auto bytes = encoded;
		const auto length = random();
		Put32(bytes, HeaderSize + 4, length);
		const auto result = Decode(bytes);
		Require(length == 8 ? result.message.has_value() : !result.message && result.error != DecodeError::None,
				"Fuzzed TLV length accepted a malformed exact envelope");
		Put32(bytes, 24, length);
		const auto envelope = Decode(bytes);
		Require(!envelope.message || Encode(*envelope.message) == bytes,
				"Fuzzed envelope length exposed a partial/noncanonical message");
	}
	for (std::size_t length = 0; length < 512; ++length)
	{
		std::vector<std::byte> arbitrary(length);
		for (auto& byte : arbitrary)
			byte = std::byte{static_cast<unsigned char>(random())};
		const auto fields = DecodeFields(arbitrary);
		const auto views = DecodeFieldViews(arbitrary);
		Require(views.error == fields.error && views.fields.size() == fields.fields.size(),
				"Borrowed decoder accepted different malformed input");
		Require(fields.error != DecodeError::None || EncodeFields(fields.fields) == arbitrary,
				"Arbitrary nested payload was not rejected or exactly preserved");
	}
}

void BorrowedFields()
{
	const auto payload = EncodeFields(std::array{Field::U64(1, UINT64_MAX, true), Field::I64(2, INT64_MIN),
												 Field::Bool(3, false), Field::Text(4, "nested text"),
												 Field::Bytes(5, EncodeFields(std::array{Field::F32(1, .25f)}))});
	const auto decoded = DecodeFieldViews(payload);
	Require(decoded.error == DecodeError::None && decoded.fields.size() == 5 && decoded.Find(1)->required &&
				decoded.Find(1)->AsU64() == UINT64_MAX && decoded.Find(2)->AsI64() == INT64_MIN &&
				decoded.Find(3)->AsBool() == false && decoded.Find(4)->AsText() == "nested text" && !decoded.Find(6),
			"Borrowed fields changed scalar decoding or metadata");
	for (const auto& field : decoded.fields)
		Require(field.value.data() >= payload.data() &&
					field.value.data() + field.value.size() <= payload.data() + payload.size(),
				"Borrowed decoder copied a field payload");
	const auto nested = DecodeFieldViews(decoded.Find(5)->value);
	Require(nested.error == DecodeError::None && nested.Find(1)->AsF32() == .25f &&
				nested.Find(1)->value.data() >= payload.data() &&
				nested.Find(1)->value.data() + 4 <= payload.data() + payload.size(),
			"Nested view lost its backing payload");
	for (std::size_t size = 0; size < payload.size(); ++size)
	{
		const auto prefix = std::span(payload).first(size);
		const auto owned = DecodeFields(prefix);
		const auto borrowed = DecodeFieldViews(prefix);
		Require(owned.error == borrowed.error && owned.fields.size() == borrowed.fields.size(),
				"Borrowed and owned decoders disagree on truncation");
	}
}

void SimulationCommandBoundaries()
{
	const std::array<unsigned char, 32> releaseHead{0x60, 0, 1, 0, 4,	 0, 0, 0, 2, 0, 0, 0, 0x94, 0, 1, 0,
													12,	  0, 0, 0, 0x93, 0, 1, 0, 4, 0, 0, 0, 0,	0, 0, 0};
	const auto encoded = EncodeSimulationCommand(simxr::ReleaseControl{simxr::ControlChannel::Head});
	Require(encoded.value.size() == releaseHead.size() &&
				std::memcmp(encoded.value.data(), releaseHead.data(), releaseHead.size()) == 0,
			"Version-one release-control command bytes changed");
	const auto decoded = DecodeSimulationCommand(encoded);
	Require(std::get<simxr::ReleaseControl>(decoded).channel == simxr::ControlChannel::Head,
			"Version-one release-control command targeted a different channel");
	const auto moving = std::get<simxr::SetHeadPose>(
		DecodeSimulationCommand(EncodeSimulationCommand(simxr::SetHeadPose{{}, {1, 2, 3}, {0, -.5f, 0}})));
	Require(moving.linearVelocity.z == 3 && moving.angularVelocity.y == -.5f && moving.angularVelocity.x == 0,
			"Head velocity did not round-trip");
	for (const auto nonfinite : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
		RequireThrows<std::invalid_argument>(
			[&] { DecodeSimulationCommand(EncodeSimulationCommand(simxr::SetGaze{{nonfinite, 0, true, true}})); },
			"Nonfinite command values reached simulation admission");
	auto envelope = DecodeFields(encoded.value).fields;
	envelope.push_back(Field::U32(65000, 1, true));
	RequireThrows<std::invalid_argument>([&] { DecodeSimulationCommand(Field::Bytes(1, EncodeFields(envelope))); },
										 "Unknown required command field was accepted");
	Message transaction;
	transaction.fields = {Field::U64(tag::TransactionId, 1), Field::U32(tag::SourceKind, 2),
						  Field::U32(tag::LatePolicy, 0),
						  Field::Bytes(tag::Commands, EncodeFields(std::array{encoded}))};
	Require(DecodeSimulationTransaction(transaction, 17).commands.size() == 1,
			"A valid bounded command transaction was rejected");
	const auto expected = EncodeSimulationCommand(simxr::SetHeadPose{});
	transaction.fields.push_back(Field::Bytes(tag::ExpectedInputs, EncodeFields(std::array{expected}), true));
	Require(DecodeSimulationTransaction(transaction, 17).expected.size() == 1,
			"Conditional simulation input did not round-trip");
	transaction.minor = 16;
	RequireThrows<std::invalid_argument>([&] { DecodeSimulationTransaction(transaction, 17); },
										 "Preconditions were accepted from a pre-1.17 request");
	transaction.minor = ProtocolMinor;
	transaction.fields.pop_back();
	auto commandWithGap = encoded;
	commandWithGap.tag = 2;
	transaction.fields.back() = Field::Bytes(tag::Commands, EncodeFields(std::array{commandWithGap}));
	RequireThrows<std::invalid_argument>([&] { DecodeSimulationTransaction(transaction, 17); },
										 "A command-list gap changed command order instead of failing");
}
} // namespace

int main()
{
	try
	{
		GoldenEnvelope();
		TypedAndNestedRecords();
		FloatRepresentations();
		UnknownFields();
		IncrementalAndHeaderFailures();
		InvalidFieldsAndLimits();
		TextValidation();
		AdversarialLengths();
		SimulationCommandBoundaries();
		BorrowedFields();
		std::cout << "Wire protocol framing, typed fields, nested bounds and adversarial lengths passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
