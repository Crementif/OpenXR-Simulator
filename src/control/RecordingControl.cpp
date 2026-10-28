#include "control/RecordingControl.h"
#include "Pch.h"
#include "runtime/Session.h"

#include <iterator>

namespace simxr::control
{
static bool SameSession(const std::weak_ptr<Session>& owner, const std::shared_ptr<Session>& session)
{
	// Comparing owners must not briefly retain another session under the recording lock.
	return !owner.owner_before(session) && !session.owner_before(owner);
}

RecordingControl::RecordingControl()
{
	m_owners.reserve(64);
}
ErrorCode RecordingControl::Start(ConnectionId connection, const std::shared_ptr<Session>& session, bool drain)
{
	if (!connection.value || !session)
		return ErrorCode::Invalid;
	const auto generation = session->Snapshot().generation;
	std::lock_guard lock(m_mutex);
	std::erase_if(m_owners,
				  [](const auto& owner)
				  {
					  return owner.session.expired();
				  });
	auto owner = std::find_if(m_owners.begin(), m_owners.end(),
							  [&](const auto& owner)
							  {
								  return SameSession(owner.session, session);
							  });
	if (owner != m_owners.end() && owner->generation != generation)
		owner->connection = {};
	if (owner != m_owners.end() && owner->connection.value &&
		(owner->connection != connection || session->Commands().Recorder().Snapshot({}, 0).active))
		return owner->connection == connection ? ErrorCode::Conflict : ErrorCode::Busy;
	if (owner == m_owners.end())
	{
		if (m_owners.size() == 64)
			return ErrorCode::Capacity;
		m_owners.push_back({session, generation, {}});
		owner = std::prev(m_owners.end());
	}
	session->StartCommandRecording(drain);
	owner->generation = generation;
	owner->connection = connection;
	return ErrorCode::Ok;
}
ErrorCode RecordingControl::Stop(ConnectionId connection, const std::shared_ptr<Session>& session,
								 std::uint64_t identity)
{
	if (!connection.value || !session)
		return ErrorCode::Invalid;
	const auto generation = session->Snapshot().generation;
	std::lock_guard lock(m_mutex);
	const auto owner = std::find_if(m_owners.begin(), m_owners.end(),
									[&](const auto& owner)
									{
										return SameSession(owner.session, session);
									});
	if (owner != m_owners.end() && owner->generation == generation && owner->connection.value &&
		owner->connection != connection)
		return ErrorCode::Busy;
	const auto state = session->Commands().Recorder().Snapshot({}, 0);
	if ((state.drain || identity) && state.identity != identity)
		return ErrorCode::Conflict;
	session->Commands().Recorder().Stop();
	if (owner != m_owners.end() && !state.drain)
		owner->connection = {};
	return ErrorCode::Ok;
}
ErrorCode RecordingControl::Acknowledge(ConnectionId connection, const std::shared_ptr<Session>& session,
										std::uint64_t identity, std::size_t through)
{
	if (!connection.value || !session || !identity)
		return ErrorCode::Invalid;
	const auto generation = session->Snapshot().generation;
	std::lock_guard lock(m_mutex);
	const auto owner = std::find_if(m_owners.begin(), m_owners.end(),
									[&](const auto& value)
									{
										return SameSession(value.session, session) && value.generation == generation;
									});
	if (owner == m_owners.end() || owner->connection != connection)
		return ErrorCode::Busy;
	const auto state = session->Commands().Recorder().Snapshot({}, 0);
	if (!state.drain || state.identity != identity)
		return ErrorCode::Conflict;
	if (through > state.total)
		return ErrorCode::Invalid;
	session->Commands().Recorder().Acknowledge(through);
	return ErrorCode::Ok;
}
void RecordingControl::Disconnect(ConnectionId connection, const std::shared_ptr<Session>& session) noexcept
{
	try
	{
		std::lock_guard lock(m_mutex);
		for (auto& owner : m_owners)
		{
			if (owner.connection != connection || !SameSession(owner.session, session))
				continue;
			session->Commands().Recorder().Stop();
			owner.connection = {};
		}
	}
	catch (...)
	{
	}
}
} // namespace simxr::control
