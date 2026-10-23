#include "control/ConnectionLeases.h"
#include "Pch.h"
#include "control/ControlRequestError.h"
#include "runtime/Session.h"

namespace simxr::control
{
ConnectionLeases::ConnectionLeases()
{
	m_leases.reserve(64);
}
ConnectionLeases::~ConnectionLeases()
{
	Close();
}
const ConnectionLeases::Lease& ConnectionLeases::Own(ControlSessionId session, DebugLeaseId lease) const
{
	if (m_closed)
		throw ControlRequestError{ErrorCode::Closed, "Connection leases are closed"};
	const auto found = std::find_if(m_leases.begin(), m_leases.end(),
									[&](const auto& entry)
									{
										return entry.session == session && entry.id == lease;
									});
	if (found == m_leases.end())
		throw ControlRequestError{ErrorCode::LeaseRequired, "Lease belongs to another connection or has expired"};
	return *found;
}
DebugLease ConnectionLeases::Acquire(ControlSessionId id, std::shared_ptr<DebugController> controller, Session& session,
									 SessionGeneration generation, Retired& retired)
{
	if (m_closed)
		throw ControlRequestError{ErrorCode::Closed, "Connection leases are closed"};
	if (!generation.value)
		throw ControlRequestError{ErrorCode::Closed, "Session has not started"};
	const auto previous = std::find_if(m_leases.begin(), m_leases.end(),
									   [&](const auto& entry)
									   {
										   return entry.session == id;
									   });
	if (previous != m_leases.end())
	{
		const auto lease = controller->Snapshot().lease;
		if (lease && lease->id == previous->id && lease->expires > session.ClockSource()->Now())
			return *lease;
		retired.m_controllers.push_back(previous->controller);
		m_leases.erase(previous);
	}
	if (m_leases.size() == 64)
		throw ControlRequestError{ErrorCode::Capacity, "Connection lease capacity exceeded"};
	const auto lease = controller->Acquire(generation);
	if (!lease)
	{
		if (session.Snapshot().generation != generation)
			throw ControlRequestError{ErrorCode::StaleGeneration, "Session generation changed"};
		throw ControlRequestError{ErrorCode::Busy, "Session has a controller or is recovering"};
	}
	m_leases.push_back({id, lease->id, std::move(controller)});
	return *lease;
}
void ConnectionLeases::Renew(ControlSessionId sessionId, DebugLeaseId lease, Session& session,
							 SessionGeneration generation)
{
	const auto& owned = Own(sessionId, lease);
	if (owned.controller->Renew(owned.id, generation))
		return;
	if (session.Snapshot().generation != generation)
		throw ControlRequestError{ErrorCode::StaleGeneration, "Session generation changed"};
	throw ControlRequestError{ErrorCode::LeaseRequired, "Lease expired"};
}
void ConnectionLeases::Relinquish(ControlSessionId sessionId, DebugLeaseId lease, Session& session,
								  SessionGeneration generation, Retired& retired)
{
	const auto& owned = Own(sessionId, lease);
	retired.m_controllers.push_back(owned.controller);
	if (!owned.controller->Relinquish(owned.id, generation) && session.Snapshot().generation != generation)
		throw ControlRequestError{ErrorCode::StaleGeneration, "Session generation changed"};
	std::erase_if(m_leases,
				  [&](const auto& entry)
				  {
					  return entry.session == sessionId;
				  });
}
DebugSubmission ConnectionLeases::Submit(ControlSessionId sessionId, DebugLeaseId lease, const DebugRequest& request)
{
	const auto& owned = Own(sessionId, lease);
	return owned.controller->Submit(owned.id, request);
}
void ConnectionLeases::Acknowledge(ControlSessionId sessionId, DebugLeaseId lease, DebugCommandId command,
								   Session& session, SessionGeneration generation, Retired& retired)
{
	const auto& owned = Own(sessionId, lease);
	if (owned.controller->Acknowledge(owned.id, command, &retired.m_operations, generation))
		return;
	if (session.Snapshot().generation != generation)
		throw ControlRequestError{ErrorCode::StaleGeneration, "Session generation changed"};
	throw ControlRequestError{ErrorCode::Busy, "Cannot acknowledge unfinished operations"};
}
void ConnectionLeases::Prune(std::span<const ControlSessionId> sessions, Retired& retired)
{
	const auto absent = [&](const Lease& lease)
	{
		return std::find(sessions.begin(), sessions.end(), lease.session) == sessions.end();
	};
	retired.m_controllers.reserve(retired.m_controllers.size() + m_leases.size());
	for (const auto& lease : m_leases)
		if (absent(lease))
			retired.m_controllers.push_back(lease.controller);
	std::erase_if(m_leases, absent);
}
void ConnectionLeases::Close() noexcept
{
	if (m_closed)
		return;
	m_closed = true;
	auto retiring = std::move(m_leases);
	for (const auto& lease : retiring)
	{
		try
		{
			lease.controller->Relinquish(lease.id);
		}
		catch (...)
		{
		}
	}
}
} // namespace simxr::control
