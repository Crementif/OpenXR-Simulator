#include "control/ConnectionRequests.h"
#include "Pch.h"

#include <stdexcept>
#include <type_traits>

namespace simxr::control
{
ConnectionRequests::ConnectionRequests(std::function<void()> onClosed) : m_onClosed(std::move(onClosed)) {}

ConnectionRequests::~ConnectionRequests()
{
	Close();
	std::optional<ControlPacket> retired;
	std::function<void()> retiredObserver;
	{
		std::lock_guard lock(m_mutex);
		retired = std::move(m_lastResponse);
		m_lastResponse.reset();
		retiredObserver = std::move(m_onClosed);
	}
	// A cached exposure can own the last native reader. Leave the mutex and
	// closed phase observable until its detached packet has been destroyed.
}

ConnectionRequests::Exchange ConnectionRequests::Begin(const ControlPacket& request)
{
	return Exchange(*this, request);
}

ConnectionRequests::Exchange::Exchange(ConnectionRequests& owner, const ControlPacket& request) : m_owner(owner)
{
	std::lock_guard lock(owner.m_mutex);
	const auto& message = request.message;
	m_minor = std::min(message.minor, ProtocolMinor);
	if (owner.m_negotiated)
		m_minor = owner.m_minor;
	if (owner.m_phase == Phase::Closing || owner.m_phase == Phase::Closed)
	{
		Reject(ErrorCode::Closed, "Connection closed");
		return;
	}
	if (owner.m_phase == Phase::Executing)
	{
		Reject(ErrorCode::Busy, "Another request is executing on this connection");
		return;
	}
	if (message.major != ProtocolMajor)
	{
		Reject(ErrorCode::UnsupportedVersion, "Unsupported protocol major");
		return;
	}
	if (request.handles.size() > MaxTransferHandles)
	{
		Reject(ErrorCode::Invalid, "Too many native handles");
		return;
	}
	m_payload = Encode(message);
	if (message.requestId <= owner.m_lastIdentity)
	{
		if (message.requestId != owner.m_lastIdentity || m_payload != owner.m_lastPayload || !owner.m_lastResponse)
		{
			Reject(ErrorCode::Conflict,
				   "Request identity retired or changed; use a new request ID with the original command ID");
			return;
		}
		if (!request.handles.empty())
		{
			Reject(ErrorCode::Conflict, "A replay cannot register another native consumer fence");
			return;
		}
		m_replay = owner.m_lastResponse;
		m_admission = Admission::Replay;
		return;
	}
	m_identity = message.requestId;
	m_admission = Admission::Execute;
	owner.m_phase = Phase::Executing;
}

ConnectionRequests::Exchange::~Exchange()
{
	if (m_admission == Admission::Execute)
		m_owner.Finish();
}

void ConnectionRequests::Exchange::Reject(ErrorCode error, const char* detail) noexcept
{
	m_error = error;
	m_detail = detail;
}

ConnectionRequests::Exchange::Admission ConnectionRequests::Exchange::Result() const noexcept
{
	return m_admission;
}

ErrorCode ConnectionRequests::Exchange::Error() const noexcept
{
	return m_error;
}

const char* ConnectionRequests::Exchange::Detail() const noexcept
{
	return m_detail;
}

std::uint16_t ConnectionRequests::Exchange::ResponseMinor() const noexcept
{
	return m_minor;
}

const ControlPacket& ConnectionRequests::Exchange::Replay() const
{
	if (m_admission != Admission::Replay)
		throw std::logic_error("Exchange has no replay response");
	return *m_replay;
}

void ConnectionRequests::Exchange::Publish(const ControlPacket& response, RetiredReplay& retired)
{
	if (m_admission != Admission::Execute || m_published || retired.m_used)
		throw std::logic_error("Exchange is not awaiting response publication");
	// Allocate the replay copy before changing the last accepted identity.
	auto cached = response;
	static_assert(std::is_nothrow_move_assignable_v<ControlPacket>);
	{
		std::lock_guard lock(m_owner.m_mutex);
		retired.m_used = true;
		retired.m_packet = std::move(m_owner.m_lastResponse);
		m_owner.m_lastPayload = std::move(m_payload);
		m_owner.m_lastResponse = std::move(cached);
		m_owner.m_lastIdentity = m_identity;
		m_published = true;
	}
}

bool ConnectionRequests::Negotiated() const
{
	std::lock_guard lock(m_mutex);
	return m_negotiated;
}

std::uint16_t ConnectionRequests::Minor() const
{
	std::lock_guard lock(m_mutex);
	return m_minor;
}

std::uint16_t ConnectionRequests::ResponseMinor(std::uint16_t requested) const
{
	std::lock_guard lock(m_mutex);
	if (m_negotiated)
		return m_minor;
	return std::min(requested, ProtocolMinor);
}

bool ConnectionRequests::Negotiate(std::uint16_t minor)
{
	std::lock_guard lock(m_mutex);
	if (m_negotiated || (m_phase != Phase::Executing && m_phase != Phase::Closing))
		return false;
	m_minor = std::min(minor, ProtocolMinor);
	m_negotiated = true;
	return true;
}

void ConnectionRequests::Close() noexcept
{
	bool notify = false;
	{
		std::lock_guard lock(m_mutex);
		switch (m_phase)
		{
		case Phase::Idle:
			m_phase = Phase::Closed;
			notify = true;
			break;
		case Phase::Executing:
			m_phase = Phase::Closing;
			break;
		case Phase::Closing:
		case Phase::Closed:
			break;
		}
	}
	if (notify)
		m_onClosed();
}

void ConnectionRequests::Finish() noexcept
{
	bool notify = false;
	{
		std::lock_guard lock(m_mutex);
		if (m_phase == Phase::Closing)
		{
			m_phase = Phase::Closed;
			notify = true;
		}
		else if (m_phase == Phase::Executing)
			m_phase = Phase::Idle;
	}
	if (notify)
		m_onClosed();
}
} // namespace simxr::control
