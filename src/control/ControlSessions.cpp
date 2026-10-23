#include "control/ControlSessions.h"
#include "Pch.h"
#include "control/ControlRequestError.h"
#include "runtime/Runtime.h"
#include "runtime/Session.h"

#include <limits>
#include <stdexcept>

namespace simxr::control
{
ControlSessions::ControlSessions(Runtime& runtime) : m_runtime(runtime) {}
std::span<const ControlSession> ControlSessions::Snapshot::Sessions() const
{
	if (m_phase != Phase::Ready)
		throw std::logic_error("Session discovery did not complete");
	return m_sessions;
}
std::span<const ControlSessionId> ControlSessions::Snapshot::Ids() const
{
	return {m_ids.data(), Sessions().size()};
}
const ControlSession& ControlSessions::Snapshot::Resolve(ControlSessionId session, SessionGeneration generation) const
{
	const auto sessions = Sessions();
	const auto found = std::find_if(sessions.begin(), sessions.end(),
									[&](const auto& entry)
									{
										return entry.id == session;
									});
	if (found == sessions.end())
		throw ControlRequestError{ErrorCode::NotFound, "Unknown session identity"};
	if (found->session->Snapshot().generation != generation)
		throw ControlRequestError{ErrorCode::StaleGeneration, "Session generation changed"};
	return *found;
}
void ControlSessions::Discover(Snapshot& snapshot)
{
	if (snapshot.m_phase != Snapshot::Phase::Empty)
		throw std::logic_error("A request can discover sessions only once");
	snapshot.m_sessions.reserve(Capacity);
	snapshot.m_phase = Snapshot::Phase::Collecting;
	std::vector<Registration> prepared;
	{
		// Keep concurrent discovery snapshots in publication order. Runtime
		// enumeration only copies handles and retained session references.
		std::lock_guard lock(m_mutex);
		for (const auto instance : m_runtime.Instances())
			for (const auto handle : m_runtime.Sessions(instance))
			{
				if (snapshot.m_sessions.size() == Capacity)
					throw ControlRequestError{ErrorCode::Capacity, "Session discovery capacity exceeded"};
				if (auto session = m_runtime.FindSession(handle))
					snapshot.m_sessions.push_back({{}, handle, session.Shared()});
			}

		prepared.reserve(Capacity);
		const auto sameOwner = [](const Registration& registered, const ControlSession& discovered)
		{
			return registered.handle == discovered.handle && !registered.session.owner_before(discovered.session) &&
				   !discovered.session.owner_before(registered.session);
		};
		for (const auto& registered : m_sessions)
			if (std::any_of(snapshot.m_sessions.begin(), snapshot.m_sessions.end(),
							[&](const auto& discovered)
							{
								return sameOwner(registered, discovered);
							}))
				prepared.push_back(registered);
		auto nextId = m_nextId;
		for (auto& discovered : snapshot.m_sessions)
		{
			const auto existing = std::find_if(prepared.begin(), prepared.end(),
											   [&](const auto& registered)
											   {
												   return sameOwner(registered, discovered);
											   });
			if (existing != prepared.end())
				discovered.id = existing->id;
			else
			{
				if (nextId == std::numeric_limits<std::uint64_t>::max())
					throw ControlRequestError{ErrorCode::Capacity, "Session identity exhausted"};
				discovered.id = {nextId++};
				prepared.push_back({discovered.id, discovered.handle, discovered.session});
			}
		}
		m_sessions.swap(prepared);
		m_nextId = nextId;
	}
	for (std::size_t index = 0; index < snapshot.m_sessions.size(); ++index)
		snapshot.m_ids[index] = snapshot.m_sessions[index].id;
	snapshot.m_phase = Snapshot::Phase::Ready;
}
} // namespace simxr::control
