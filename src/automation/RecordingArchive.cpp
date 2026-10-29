#include "automation/RecordingArchive.h"
#include "Pch.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"

namespace simxr::automation
{
static constexpr std::array<char, 8> s_magic{'S', 'I', 'M', 'X', 'R', 'T', 'K', '1'};
static constexpr std::array<char, 8> s_gameMagic{'S', 'I', 'M', 'X', 'R', 'T', 'K', '2'};

static std::uint32_t Checksum(std::span<const std::byte> bytes)
{
	std::uint32_t value = ~0u;
	for (const auto byte : bytes)
	{
		value ^= std::to_integer<unsigned char>(byte);
		for (unsigned bit = 0; bit < 8; ++bit)
			value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0);
	}
	return ~value;
}

static void Put(std::span<char, 4> bytes, std::uint32_t value)
{
	for (unsigned index = 0; index < 4; ++index)
		bytes[index] = static_cast<char>(value >> (index * 8));
}

static std::uint32_t Get(std::span<const char, 4> bytes)
{
	std::uint32_t value = 0;
	for (unsigned index = 0; index < 4; ++index)
		value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index])) << (index * 8);
	return value;
}

static bool SameOrigin(const InputRecording& first, const InputRecording& next)
{
	if (first.seed != next.seed || first.originFrame != next.originFrame || first.originTime != next.originTime ||
		first.originRuntimeTime != next.originRuntimeTime || first.initialScenarios != next.initialScenarios ||
		first.initial.size() != next.initial.size())
		return false;
	for (std::size_t index = 0; index < first.initial.size(); ++index)
		if (control::EncodeSimulationCommand(first.initial[index]).value !=
			control::EncodeSimulationCommand(next.initial[index]).value)
			return false;
	return true;
}

static RecordingPage Page(const control::Message& message, control::OpCode opcode)
{
	const auto error = message.Find(control::tag::Error);
	if (message.opcode != static_cast<std::uint16_t>(opcode) || !error || error->AsU32() != 0)
		throw std::runtime_error("Archive requires a successful recording response");
	auto page = DecodeRecordingPage(message);
	if (!page.drain || !page.identity || page.recording.version < 3 || page.recording.version > 4 ||
		page.recording.transactions.size() > 16)
		throw std::runtime_error("Archive requires a bounded schema-3/4 draining page");
	return page;
}

static RecordingArchiveState Initial(const control::Message& message)
{
	auto page = Page(message, control::OpCode::StartRecording);
	if (page.offset || page.floor || !page.recording.transactions.empty() || !page.recording.active ||
		page.recording.overflow)
		throw std::runtime_error("Archive must start with a fresh recording origin");
	RecordingArchiveState result;
	result.origin = std::move(page.recording);
	result.identity = page.identity;
	const auto session = message.Find(control::tag::Session), generation = message.Find(control::tag::Generation);
	result.session = session ? session->AsU64().value_or(0) : 0;
	result.generation = generation ? generation->AsU64().value_or(0) : 0;
	return result;
}

static RecordingArchiveState Advance(const RecordingArchiveState& state, const RecordingPage& page, bool finish)
{
	if (state.complete || state.truncated || page.identity != state.identity || page.offset != state.next ||
		page.floor > state.next || page.recording.total < state.origin.total ||
		page.recording.version < state.origin.version || (state.origin.overflow && !page.recording.overflow) ||
		(!state.origin.active && page.recording.active) || !SameOrigin(state.origin, page.recording) ||
		(finish &&
		 (page.recording.active || !page.recording.transactions.empty() || page.recording.total != state.next)))
		throw std::runtime_error("Archive page changed identity, origin, order or completion state");
	auto next = state;
	next.next += page.recording.transactions.size(); // Decoder already checked against the uint64 total.
	next.origin.version = page.recording.version;
	next.origin.total = page.recording.total;
	next.origin.active = page.recording.active;
	next.origin.overflow = page.recording.overflow;
	next.complete = finish && !page.recording.overflow &&
					(!state.game || (state.game->complete && !state.game->gap && !state.game->failed));
	return next;
}

static control::Message Footer(const control::Message& message, std::uint64_t next)
{
	auto footer = message;
	std::erase_if(footer.fields, [](const auto& field) {
		return field.tag == control::tag::Offset || field.tag == control::tag::Length || field.tag == control::tag::End;
	});
	footer.fields.push_back(control::Field::U64(control::tag::Offset, next));
	footer.fields.push_back(control::Field::U64(control::tag::Length, 0));
	footer.fields.push_back(control::Field::Bool(control::tag::End, true));
	return footer;
}

static GameRecordingState AdvanceGame(const std::optional<GameRecordingState>& state, const RecordedGamePage& page,
									  std::string_view phase)
{
	if (!page.session || !page.generation)
		throw std::runtime_error("SDK capture lacks a session identity");
	GameRecordingState next;
	if (phase == "begin")
	{
		if (state || !page.journal.observations.empty())
			throw std::runtime_error("Invalid SDK capture origin");
		next.origin = page;
		next.next = page.journal.total;
	}
	else
	{
		if (!state || state->complete || page.session != state->origin.session ||
			page.generation != state->origin.generation || page.journal.total < state->total ||
			page.journal.offset != std::max(state->next, page.journal.floor) ||
			page.journal.gap != (state->next < page.journal.floor))
			throw std::runtime_error("SDK archive changed identity or journal order");
		next = *state;
		next.next = page.journal.next;
		if (phase == "end")
		{
			if (!page.journal.observations.empty())
				throw std::runtime_error("SDK archive footer contains observations");
			next.complete = true;
		}
		else if (phase != "page")
			throw std::runtime_error("Unknown SDK archive phase");
	}
	next.total = page.journal.total;
	next.gap |= page.journal.gap;
	next.failed |= page.journal.failed;
	next.uncorrelated |= !page.clock.has_value();
	if (page.clock && next.origin.clock)
		next.uncorrelated |=
			page.clock->revision != next.origin.clock->revision || page.clock->domain != next.origin.clock->domain;
	for (const auto& observation : page.journal.observations)
		next.uncorrelated |= !observation.time || !page.clock || observation.time->domain != page.clock->domain;
	return next;
}

static std::string_view GamePhase(const control::Message& message)
{
	const auto phase = message.Find(control::tag::Phase);
	if (!phase || !phase->AsText())
		throw std::runtime_error("Missing SDK archive phase");
	return *phase->AsText();
}

RecordingArchiveWriter::RecordingArchiveWriter(const std::filesystem::path& directory, const control::Message& start,
											   const std::optional<control::Message>& gameOrigin)
	: m_state(Initial(start))
{
	if (gameOrigin)
	{
		const auto page = DecodeRecordedGamePage(*gameOrigin);
		if (!m_state.session || page.session != m_state.session || page.generation != m_state.generation)
			throw std::runtime_error("SDK origin does not belong to the input capture");
		(void)AdvanceGame({}, page, "begin");
	}
	if (!std::filesystem::create_directory(directory))
		throw std::runtime_error("Recording archive directory already exists");
	m_output.open(directory / "input.simxr", std::ios::binary | std::ios::out);
	m_gameFormat = gameOrigin.has_value();
	const auto& magic = gameOrigin ? s_gameMagic : s_magic;
	m_output.write(magic.data(), magic.size());
	Write(start);
	if (gameOrigin)
		AppendGame(*gameOrigin, "begin");
}

void RecordingArchiveWriter::Write(const control::Message& message)
{
	if (m_failed || !m_output.is_open())
		throw std::runtime_error("Recording archive writer is closed or failed");
	auto normalized = message;
	normalized.requestId = control::RequestId{1};
	const auto bytes = control::Encode(normalized);
	std::array<char, 8> header;
	Put(std::span<char, 8>(header).first<4>(), static_cast<std::uint32_t>(bytes.size()));
	Put(std::span<char, 8>(header).last<4>(), Checksum(bytes));
	m_output.write(header.data(), header.size());
	m_output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	m_output.flush();
	if (!m_output)
	{
		m_failed = true;
		throw std::runtime_error("Cannot write and flush recording archive page");
	}
}

std::uint64_t RecordingArchiveWriter::Append(const control::Message& message)
{
	const auto page = Page(message, control::OpCode::ReadRecording);
	auto next = Advance(m_state, page, false);
	Write(message);
	m_state = std::move(next);
	return m_state.next;
}

void RecordingArchiveWriter::AppendGame(const control::Message& page, std::string_view phase)
{
	if (!m_gameFormat || m_state.complete || (phase != "begin" && !m_state.game))
		throw std::runtime_error("SDK archive is not open");
	auto message = page;
	std::erase_if(message.fields, [](const auto& field) { return field.tag == control::tag::Phase; });
	message.fields.push_back(control::Field::Text(control::tag::Phase, phase));
	auto next = AdvanceGame(m_state.game, DecodeRecordedGamePage(message), phase);
	Write(message);
	m_state.game = std::move(next);
}

void RecordingArchiveWriter::Finish(const control::Message& stopped)
{
	const auto footer = Footer(stopped, m_state.next);
	auto next = Advance(m_state, Page(footer, control::OpCode::StopRecording), true);
	Write(footer);
	m_output.close();
	if (!m_output)
	{
		m_failed = true;
		throw std::runtime_error("Cannot close recording archive");
	}
	m_state = std::move(next);
}

RecordingArchiveReader::RecordingArchiveReader(const std::filesystem::path& directory)
	: m_input(directory / "input.simxr", std::ios::binary)
{
	std::array<char, 8> magic;
	if (!m_input.read(magic.data(), magic.size()) || (magic != s_magic && magic != s_gameMagic))
		throw std::runtime_error("Invalid recording archive header");
	m_gameFormat = magic == s_gameMagic;
	const auto start = Read();
	if (!start)
		throw std::runtime_error("Recording archive has no complete origin");
	m_state = Initial(*start);
	if (m_gameFormat)
	{
		const auto begin = Read();
		if (!begin || GamePhase(*begin) != "begin")
			throw std::runtime_error("SDK archive has no complete origin");
		const auto page = DecodeRecordedGamePage(*begin);
		if (!m_state.session || page.session != m_state.session || page.generation != m_state.generation)
			throw std::runtime_error("SDK origin does not belong to the input capture");
		m_state.game = AdvanceGame({}, page, "begin");
	}
}

static void CloseInput(std::ifstream& input)
{
	input.clear();
	input.close();
	if (!input)
		throw std::runtime_error("Cannot close recording archive reader");
}

std::optional<control::Message> RecordingArchiveReader::Read()
{
	std::array<char, 8> header;
	m_input.read(header.data(), header.size());
	const auto count = m_input.gcount();
	if (m_input.bad() || (!m_input && !m_input.eof()))
		throw std::runtime_error("Cannot read recording archive header");
	if (count != static_cast<std::streamsize>(header.size()))
	{
		CloseInput(m_input);
		m_state.truncated = count != 0;
		m_end = true;
		return {};
	}
	const auto length = Get(std::span<const char, 8>(header).first<4>());
	if (length < control::HeaderSize || length > control::MaxPayload + control::HeaderSize)
		throw std::runtime_error("Recording archive page exceeds its bound");
	std::vector<std::byte> bytes(length);
	m_input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	if (!m_input)
	{
		if (m_input.bad() || !m_input.eof())
			throw std::runtime_error("Cannot read recording archive page");
		CloseInput(m_input);
		m_state.truncated = m_end = true;
		return {};
	}
	if (Checksum(bytes) != Get(std::span<const char, 8>(header).last<4>()))
		throw std::runtime_error("Recording archive checksum mismatch");
	auto decoded = control::Decode(bytes);
	if (!decoded.message)
		throw std::runtime_error("Malformed recording archive page");
	return std::move(decoded.message);
}

std::optional<RecordingPage> RecordingArchiveReader::Next(std::stop_token stop)
{
	while (true)
	{
		if (stop.stop_requested())
			throw std::runtime_error("Recording archive read cancelled");
		auto event = NextEvent();
		if (!event)
			return {};
		if (auto* page = std::get_if<RecordingPage>(&*event))
			return std::move(*page);
	}
}

std::optional<RecordingArchiveEvent> RecordingArchiveReader::NextEvent()
{
	if (m_end)
		return {};
	const auto message = Read();
	if (!message)
		return {};
	if (message->opcode == static_cast<std::uint16_t>(control::OpCode::ReadGameObservations))
	{
		if (!m_gameFormat)
			throw std::runtime_error("Legacy input archive contains SDK records");
		auto page = DecodeRecordedGamePage(*message);
		m_state.game = AdvanceGame(m_state.game, page, GamePhase(*message));
		return RecordingArchiveEvent{std::move(page)};
	}
	const bool finish = message->opcode == static_cast<std::uint16_t>(control::OpCode::StopRecording);
	auto page = Page(*message, finish ? control::OpCode::StopRecording : control::OpCode::ReadRecording);
	auto next = Advance(m_state, page, finish);
	if (finish)
	{
		if (m_input.peek() != std::char_traits<char>::eof() || m_input.bad())
			throw std::runtime_error("Data follows recording archive completion");
		CloseInput(m_input);
		m_end = true;
	}
	m_state = std::move(next);
	return finish ? std::nullopt : std::optional<RecordingArchiveEvent>{std::move(page)};
}
} // namespace simxr::automation
