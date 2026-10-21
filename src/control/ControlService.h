#pragma once

#include "control/ControlPacket.h"

#include <cstddef>
#include <memory>

namespace simxr
{
class Runtime;
}
namespace simxr::control
{
class ImageInspectionProvider;
// Transport workers call this service without holding runtime/native locks.
// The runtime outlives the service; Close disconnects leases before its teardown.
class ControlService
{
  public:
	explicit ControlService(Runtime& runtime, std::size_t maxConnections = 16,
							std::shared_ptr<ImageInspectionProvider> images = {});
	~ControlService();
	ControlService(const ControlService&) = delete;
	ControlService& operator=(const ControlService&) = delete;
	std::optional<ConnectionId> Connect(bool nativeHandles = false);
	void Disconnect(ConnectionId connection) noexcept;
	Message Handle(ConnectionId connection, const Message& request);
	ControlPacket HandlePacket(ConnectionId connection, const ControlPacket& request);
	void Close() noexcept;

  private:
	class State;
	std::unique_ptr<State> m_state;
};
} // namespace simxr::control
