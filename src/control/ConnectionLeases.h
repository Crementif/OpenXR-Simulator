#pragma once

#include "control/ProtocolTypes.h"
#include "presentation/DebugController.h"

#include <memory>
#include <span>
#include <vector>

namespace simxr::control
{
// The connection's admission gate serializes use. Retired readers outlive it.
class ConnectionLeases
{
  public:
	class Retired
	{
		friend class ConnectionLeases;
		std::vector<std::shared_ptr<DebugController>> m_controllers;
		std::vector<DebugOperation> m_operations;
	};
	ConnectionLeases();
	~ConnectionLeases();
	ConnectionLeases(const ConnectionLeases&) = delete;
	ConnectionLeases& operator=(const ConnectionLeases&) = delete;
	DebugLease Acquire(ControlSessionId id, std::shared_ptr<DebugController> controller, Session& session,
					   SessionGeneration generation, Retired& retired);
	void Renew(ControlSessionId sessionId, DebugLeaseId lease, Session& session, SessionGeneration generation);
	void Relinquish(ControlSessionId sessionId, DebugLeaseId lease, Session& session, SessionGeneration generation,
					Retired& retired);
	DebugSubmission Submit(ControlSessionId sessionId, DebugLeaseId lease, const DebugRequest& request);
	void Acknowledge(ControlSessionId sessionId, DebugLeaseId lease, DebugCommandId command, Session& session,
					 SessionGeneration generation, Retired& retired);
	void Prune(std::span<const ControlSessionId> sessions, Retired& retired);
	void Close() noexcept;

  private:
	struct Lease
	{
		ControlSessionId session;
		DebugLeaseId id;
		std::shared_ptr<DebugController> controller;
	};
	const Lease& Own(ControlSessionId session, DebugLeaseId lease) const;
	std::vector<Lease> m_leases;
	bool m_closed = false;
};
} // namespace simxr::control
