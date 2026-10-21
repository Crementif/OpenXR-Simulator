#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <simxr/game_bridge.h>
#include <string>
#include <vector>

namespace simxr
{
enum class GameCommandPhase
{
	Queued,
	Delivered,
	Complete,
	Failed,
	Cancelled,
	Interrupted
};
struct GameCommandRecord
{
	std::uint64_t id = 0, source = 0, clientId = 0;
	std::uint32_t kind = 0;
	std::string payload, result;
	GameCommandPhase phase = GameCommandPhase::Queued;
	bool sourceDisconnected = false;
};
struct GameBridgeSnapshot
{
	std::uint64_t game = 0, revision = 0;
	std::uint32_t capabilities = 0;
	std::string telemetry;
	std::size_t queued = 0, delivered = 0;
};
class GameBridge
{
  public:
	GameBridge();
	std::uint64_t Register(std::uint32_t capabilities);
	void Close() noexcept;
	void Unregister(std::uint64_t game) noexcept;
	std::optional<GameCommandRecord> Submit(std::uint64_t game, std::uint64_t source, std::uint64_t clientId,
											std::uint32_t kind, std::string payload);
	std::optional<GameCommandRecord> Read(std::uint64_t game, std::uint64_t source, std::uint64_t id) const;
	bool Poll(std::uint64_t game, SimxrGameCommand& command);
	bool Complete(std::uint64_t game, std::uint64_t id, bool success, std::string result);
	bool Publish(std::uint64_t game, std::string telemetry);
	void Disconnect(std::uint64_t source, bool retire = false) noexcept;
	GameBridgeSnapshot Snapshot() const;
	bool FillApi(std::uint32_t version, SimxrGameBridgeApi& api) noexcept;

  private:
	mutable std::mutex m_mutex;
	GameBridgeSnapshot m_state;
	bool m_closed = false;
	std::uint64_t m_nextGame = 1, m_nextCommand = 1, m_lastGame = 0;
	struct SourceCursor
	{
		std::uint64_t source = 0, last = 0;
	};
	std::vector<SourceCursor> m_sources;
	std::vector<GameCommandRecord> m_commands;
};
} // namespace simxr
