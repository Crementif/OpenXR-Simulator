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
class RecordingControl;
struct ControlSession;

// Used under the enclosing connection's single-flight gate. Destruction runs
// after that gate and its mutex are released; recordings outlives this owner.
class SimulationConnection
{
  public:
	SimulationConnection(ConnectionId connection, RecordingControl& recordings, GameBridge& game);
	~SimulationConnection();
	SimulationConnection(const SimulationConnection&) = delete;
	SimulationConnection& operator=(const SimulationConnection&) = delete;
	Message Dispatch(const Message& request, const ControlSession& binding, Message response,
					 std::vector<SnapshotRecord>& held);
	bool OwnsSequenceCapture(const SnapshotRecord& capture) const noexcept;

  private:
	std::optional<CommandSource> Source(std::optional<CommandSourceKind> requested) const;
	CommandSource RequestSource(const Message& request) const;
	void Track(const std::shared_ptr<Session>& session, CommandSource source);
	struct Use
	{
		std::shared_ptr<Session> session;
		CommandSource source;
	};
	ConnectionId m_connection;
	RecordingControl& m_recordings;
	GameBridge& m_game;
	std::optional<CommandSourceKind> m_kind;
	std::vector<Use> m_uses;
	SequenceControl m_sequences;
};
} // namespace simxr::control
