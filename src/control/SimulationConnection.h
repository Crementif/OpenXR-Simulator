#pragma once

#include "control/ProtocolTypes.h"
#include "control/SequenceControl.h"
#include "control/WireProtocol.h"
#include "simulation/CommandTypes.h"

#include <memory>
#include <optional>
#include <vector>

namespace simxr
{
class Session;
class GameBridge;
} // namespace simxr
namespace simxr::control
{
struct ControlSession;

// Used under the enclosing connection's single-flight gate. Destruction runs
// after that gate and its mutex are released.
class SimulationConnection
{
  public:
	SimulationConnection(ConnectionId connection, GameBridge& game);
	~SimulationConnection();
	SimulationConnection(const SimulationConnection&) = delete;
	SimulationConnection& operator=(const SimulationConnection&) = delete;
	Message Dispatch(const Message& request, const ControlSession& binding, Message response,
					 std::vector<SnapshotRecord>& held);
	bool OwnsSequenceCapture(const SnapshotRecord& capture) const noexcept;

  private:
	Message DispatchLiveInput(const Message& request, const ControlSession& binding, Message response);
	std::optional<CommandSource> Source(std::optional<CommandSourceKind> requested) const;
	CommandSource RequestSource(const Message& request) const;
	void Track(const std::shared_ptr<Session>& session, CommandSource source);
	struct Use
	{
		std::shared_ptr<Session> session;
		CommandSource source;
	};
	ConnectionId m_connection;
	GameBridge& m_game;
	std::optional<CommandSourceKind> m_kind;
	std::vector<Use> m_uses;
	SequenceControl m_sequences;
	std::shared_ptr<Session> m_liveSession;
	SessionGeneration m_liveGeneration;
	std::uint64_t m_attachment = 0;
};
} // namespace simxr::control
