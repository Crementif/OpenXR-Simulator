#include "simulation/GameBridge.h"
#include "Pch.h"

namespace simxr
{
static void ValidateText(std::string_view text)
{
	if (text.size() > SIMXR_GAME_PAYLOAD_CAPACITY || text.find('\0') != std::string_view::npos)
		throw std::invalid_argument("Game bridge text exceeds its bounded payload");
	// UTF-8 validation also prevents telemetry from breaking JSON/protocol output.
	std::uint32_t value = 0, minimum = 0;
	unsigned pending = 0;
	for (const unsigned char byte : text)
	{
		if (pending)
		{
			if ((byte & 0xc0) != 0x80)
				throw std::invalid_argument("Invalid game bridge UTF-8");
			value = (value << 6) | (byte & 0x3f);
			if (!--pending && (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)))
				throw std::invalid_argument("Invalid game bridge UTF-8");
		}
		else if (byte < 0x80)
			continue;
		else if ((byte & 0xe0) == 0xc0)
		{
			value = byte & 0x1f;
			pending = 1;
			minimum = 0x80;
		}
		else if ((byte & 0xf0) == 0xe0)
		{
			value = byte & 0x0f;
			pending = 2;
			minimum = 0x800;
		}
		else if ((byte & 0xf8) == 0xf0)
		{
			value = byte & 7;
			pending = 3;
			minimum = 0x10000;
		}
		else
			throw std::invalid_argument("Invalid game bridge UTF-8");
	}
	if (pending)
		throw std::invalid_argument("Truncated game bridge UTF-8");
}
static bool Terminal(GameCommandPhase phase)
{
	return phase != GameCommandPhase::Queued && phase != GameCommandPhase::Delivered;
}

GameBridge::GameBridge(std::size_t observationCapacity)
{
	if (!observationCapacity || observationCapacity > 4096)
		throw std::invalid_argument("Game observation capacity must be in [1, 4096]");
	m_observations.resize(observationCapacity);
	m_commands.reserve(128);
	m_sources.reserve(64);
}
void GameBridge::Observe(GameObservation observation) noexcept
{
	if (m_observationNext == std::numeric_limits<std::uint64_t>::max())
	{
		m_observationFailed = true;
		return;
	}
	observation.sequence = m_observationNext;
	observation.game = m_state.game;
	observation.revision = m_state.revision;
	observation.capabilities = m_state.capabilities;
	try
	{
		observation.time = ReadNativeClockSample();
	}
	catch (...)
	{
		// Keep the event, but do not invent a timestamp when correlation fails.
	}
	m_observations[m_observationNext % m_observations.size()] = std::move(observation);
	++m_observationNext;
}
GameObservationPage GameBridge::Observations(std::optional<std::uint64_t> offset, std::size_t count) const
{
	if (count > 16)
		throw std::invalid_argument("Game observation pages contain at most 16 records");
	std::lock_guard lock(m_mutex);
	GameObservationPage page;
	page.state = m_state;
	page.total = m_observationNext;
	page.floor = page.total > m_observations.size() ? page.total - m_observations.size() : 0;
	if (offset && *offset > page.total)
		throw std::invalid_argument("Game observation offset is beyond the journal");
	page.offset = std::max(offset.value_or(page.floor), page.floor);
	page.gap = offset && *offset < page.floor;
	page.failed = m_observationFailed;
	page.next = page.offset + std::min<std::uint64_t>(count, page.total - page.offset);
	page.observations.reserve(static_cast<std::size_t>(page.next - page.offset));
	for (auto cursor = page.offset; cursor < page.next; ++cursor)
		page.observations.push_back(*m_observations[cursor % m_observations.size()]);
	return page;
}
std::uint64_t GameBridge::Register(std::uint32_t capabilities)
{
	if (!capabilities || (capabilities & ~7U))
		return 0;
	std::lock_guard lock(m_mutex);
	if (m_closed || m_state.game || m_nextGame == std::numeric_limits<std::uint64_t>::max())
		return 0;
	m_state = {};
	m_state.game = m_nextGame++;
	m_state.capabilities = capabilities;
	m_commands.clear();
	m_sources.clear();
	m_lastGame = m_state.game;
	GameObservation registered;
	registered.kind = GameObservationKind::Registered;
	Observe(std::move(registered));
	return m_state.game;
}
void GameBridge::Close() noexcept
{
	std::lock_guard lock(m_mutex);
	if (m_closed)
		return;
	if (m_state.game)
	{
		GameObservation closed;
		closed.kind = GameObservationKind::Closed;
		Observe(std::move(closed));
	}
	m_closed = true;
	for (auto& command : m_commands)
		if (!Terminal(command.phase))
			command.phase = GameCommandPhase::Interrupted;
	m_state.capabilities = 0;
	m_state.game = 0;
}
void GameBridge::Unregister(std::uint64_t game) noexcept
{
	std::lock_guard lock(m_mutex);
	if (!game || m_state.game != game)
		return;
	GameObservation unregistered;
	unregistered.kind = GameObservationKind::Unregistered;
	Observe(std::move(unregistered));
	for (auto& command : m_commands)
		if (!Terminal(command.phase))
			command.phase = GameCommandPhase::Interrupted;
	m_state.capabilities = 0;
	m_state.game = 0;
}
std::optional<GameCommandRecord> GameBridge::Submit(std::uint64_t game, std::uint64_t source, std::uint64_t clientId,
													std::uint32_t kind, std::string payload)
{
	ValidateText(payload);
	if (!source || !clientId || kind < 1 || kind > 3)
		return {};
	std::lock_guard lock(m_mutex);
	if (!game || m_state.game != game || !(m_state.capabilities & (1U << (kind - 1))))
		return {};
	for (const auto& command : m_commands)
		if (command.source == source && command.clientId == clientId)
		{
			if (command.kind != kind || command.payload != payload)
				return {};
			return command;
		}
	auto cursor = std::find_if(m_sources.begin(), m_sources.end(),
							   [&](const auto& cursor)
							   {
								   return cursor.source == source;
							   });
	if ((cursor == m_sources.end() && m_sources.size() == 64) ||
		(cursor != m_sources.end() && clientId <= cursor->last))
		return {};
	if (m_nextCommand == std::numeric_limits<std::uint64_t>::max())
		return {};
	if (m_commands.size() == 128)
	{
		const auto found = std::find_if(m_commands.begin(), m_commands.end(),
										[](const auto& command)
										{
											return Terminal(command.phase);
										});
		if (found == m_commands.end())
			return {};
		m_commands.erase(found);
	}
	GameCommandRecord command{m_nextCommand, source, clientId, kind, std::move(payload)};
	auto result = command;
	m_commands.push_back(std::move(command));
	++m_nextCommand;
	if (cursor == m_sources.end())
		m_sources.push_back({source, clientId});
	else
		cursor->last = clientId;
	return result;
}
std::optional<GameCommandRecord> GameBridge::Read(std::uint64_t game, std::uint64_t source, std::uint64_t id) const
{
	std::lock_guard lock(m_mutex);
	if (game != m_state.game && (m_state.game || game != m_lastGame))
		return {};
	for (const auto& command : m_commands)
		if (command.source == source && command.id == id)
			return command;
	return {};
}
bool GameBridge::Poll(std::uint64_t game, SimxrGameCommand& output)
{
	std::lock_guard lock(m_mutex);
	if (game != m_state.game || !game)
		return false;
	for (auto& command : m_commands)
		if (command.phase == GameCommandPhase::Queued)
		{
			output = {};
			output.id = command.id;
			output.kind = command.kind;
			output.payloadLength = static_cast<std::uint32_t>(command.payload.size());
			std::copy(command.payload.begin(), command.payload.end(), output.payload);
			command.phase = GameCommandPhase::Delivered;
			return true;
		}
	return false;
}
bool GameBridge::Complete(std::uint64_t game, std::uint64_t id, bool success, std::string result)
{
	ValidateText(result);
	std::lock_guard lock(m_mutex);
	if (!game || m_state.game != game)
		return false;
	for (auto& command : m_commands)
		if (command.id == id && command.phase == GameCommandPhase::Delivered)
		{
			command.result = std::move(result);
			command.phase = success ? GameCommandPhase::Complete : GameCommandPhase::Failed;
			return true;
		}
	return false;
}
bool GameBridge::Publish(std::uint64_t game, std::string telemetry)
{
	ValidateText(telemetry);
	std::lock_guard lock(m_mutex);
	if (!game || m_state.game != game || m_state.revision == std::numeric_limits<std::uint64_t>::max())
		return false;
	GameObservation observation;
	observation.payload = telemetry;
	m_state.telemetry = std::move(telemetry);
	++m_state.revision;
	Observe(std::move(observation));
	return true;
}
void GameBridge::Disconnect(std::uint64_t source, bool retire) noexcept
{
	std::lock_guard lock(m_mutex);
	for (auto& command : m_commands)
		if (command.source == source)
		{
			command.sourceDisconnected = true;
			if (command.phase == GameCommandPhase::Queued)
				command.phase = GameCommandPhase::Cancelled;
		}
	if (retire)
		std::erase_if(m_sources,
					  [&](const auto& cursor)
					  {
						  return cursor.source == source;
					  });
}
GameBridgeSnapshot GameBridge::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	auto result = m_state;
	for (const auto& command : m_commands)
	{
		result.queued += command.phase == GameCommandPhase::Queued;
		result.delivered += command.phase == GameCommandPhase::Delivered;
	}
	return result;
}
bool GameBridge::FillApi(std::uint32_t version, SimxrGameBridgeApi& api) noexcept
{
	if (version != SIMXR_GAME_BRIDGE_VERSION || api.size < sizeof(api))
		return false;
	api = {};
	api.version = version;
	api.size = sizeof(api);
	api.context = this;
	api.registerGame = [](void* context, std::uint32_t capabilities) -> std::uint64_t
	{
		try
		{
			return static_cast<GameBridge*>(context)->Register(capabilities);
		}
		catch (...)
		{
			return 0;
		}
	};
	api.unregisterGame = [](void* context, std::uint64_t game)
	{
		try
		{
			static_cast<GameBridge*>(context)->Unregister(game);
		}
		catch (...)
		{
		}
	};
	api.poll = [](void* context, std::uint64_t game, SimxrGameCommand* command) -> int
	{
		try
		{
			return command && static_cast<GameBridge*>(context)->Poll(game, *command);
		}
		catch (...)
		{
			return 0;
		}
	};
	api.complete = [](void* context, std::uint64_t game, std::uint64_t command, int success, const char* result,
					  std::uint32_t length) -> int
	{
		try
		{
			return length <= SIMXR_GAME_PAYLOAD_CAPACITY && (result || !length) &&
				   static_cast<GameBridge*>(context)->Complete(game, command, success != 0,
															   std::string{result ? result : "", length});
		}
		catch (...)
		{
			return 0;
		}
	};
	api.publish = [](void* context, std::uint64_t game, const char* telemetry, std::uint32_t length) -> int
	{
		try
		{
			return length <= SIMXR_GAME_PAYLOAD_CAPACITY && (telemetry || !length) &&
				   static_cast<GameBridge*>(context)->Publish(game, std::string{telemetry ? telemetry : "", length});
		}
		catch (...)
		{
			return 0;
		}
	};
	api.shutdown = [](void* context) -> int
	{
		try
		{
			static_cast<GameBridge*>(context)->Close();
			return 1;
		}
		catch (...)
		{
			return 0;
		}
	};
	return true;
}
} // namespace simxr
