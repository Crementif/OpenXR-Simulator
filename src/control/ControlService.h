#pragma once

#include "control/ControlPacket.h"
#include "control/ControlSessions.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace simxr
{
class Runtime;
} // namespace simxr

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
	struct Client;
	struct Publication;
	struct HeldReaders;

	std::uint64_t ServiceCapabilities(std::uint16_t minor) const;
	std::uint64_t AdvertisedCapabilities(const Client& client, std::uint16_t minor) const;
	std::shared_ptr<Client> Find(ConnectionId id);
	const ControlSession& Resolve(Client& client, const Message& request, HeldReaders& held);
	Message DispatchGame(Client& client, const Message& request);
	Message DispatchWindow(Client& client, const Message& request, HeldReaders& held, Publication& publication,
						   Message response);
	Message DispatchFrame(Client& client, const Message& request, const ControlSession& binding, HeldReaders& held,
						  Publication& publication, Message response);
	Message DispatchDebugger(Client& client, const Message& request, const ControlSession& binding, HeldReaders& held,
							 Message response);
	Message Dispatch(Client& client, const Message& request, HeldReaders& held, Publication& publication,
					 const ControlPacket& incoming, ControlPacket& outgoing);

	Runtime& m_runtime;
	const std::shared_ptr<ImageInspectionProvider> m_images;
	const std::size_t m_capacity;
	std::mutex m_mutex;
	bool m_closed = false;
	std::uint64_t m_nextConnection = 1;
	ControlSessions m_sessions;
	std::vector<std::pair<ConnectionId, std::shared_ptr<Client>>> m_clients;
};
} // namespace simxr::control
