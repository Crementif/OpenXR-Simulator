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
	ErrorCode Start(ConnectionId connection, const std::shared_ptr<Session>& session);
	ErrorCode Stop(ConnectionId connection, const std::shared_ptr<Session>& session);
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
