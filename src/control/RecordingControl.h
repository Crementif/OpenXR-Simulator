#pragma once

#include "control/ProtocolTypes.h"
#include "presentation/FrameTypes.h"

#include <memory>
#include <mutex>
#include <vector>

namespace simxr
{
class Session;
}
namespace simxr::control
{
class RecordingControl
{
  public:
	RecordingControl();
	ErrorCode Start(ConnectionId connection, const std::shared_ptr<Session>& session, bool drain = false);
	ErrorCode Stop(ConnectionId connection, const std::shared_ptr<Session>& session, std::uint64_t identity = 0);
	ErrorCode Acknowledge(ConnectionId connection, const std::shared_ptr<Session>& session, std::uint64_t identity,
						  std::size_t through);
	void Disconnect(ConnectionId connection, const std::shared_ptr<Session>& session) noexcept;

  private:
	struct Owner
	{
		std::weak_ptr<Session> session;
		SessionGeneration generation;
		ConnectionId connection;
	};
	std::mutex m_mutex;
	std::vector<Owner> m_owners;
};
} // namespace simxr::control
