#pragma once

#include "control/ControlPacket.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace simxr::control
{
// Owns one connection's negotiation, single-flight admission and exact-envelope
// replay. No mutex spans dispatch, close notification or native-reader release.
class ConnectionRequests
{
public:
	class RetiredReplay
	{
		friend class ConnectionRequests;
		std::optional<ControlPacket> m_packet;
		bool m_used = false;
	};

	class Exchange
	{
		friend class ConnectionRequests;

	public:
		enum class Admission
		{
			Execute,
			Replay,
			Reject
		};
		~Exchange();
		Exchange(const Exchange&) = delete;
		Exchange& operator=(const Exchange&) = delete;
		Admission Result() const noexcept;
		ErrorCode Error() const noexcept;
		const char* Detail() const noexcept;
		std::uint16_t ResponseMinor() const noexcept;
		const ControlPacket& Replay() const;
		// Copies before committing. Replaced packets remain in retired until the
		// caller has left this Exchange and reopened admission.
		void Publish(const ControlPacket& response, RetiredReplay& retired);

	private:
		Exchange(ConnectionRequests& owner, const ControlPacket& request);
		void Reject(ErrorCode error, const char* detail) noexcept;
		ConnectionRequests& m_owner;
		Admission m_admission = Admission::Reject;
		ErrorCode m_error = ErrorCode::Internal;
		const char* m_detail = nullptr;
		std::uint16_t m_minor = 0;
		RequestId m_identity;
		std::vector<std::byte> m_payload;
		std::optional<ControlPacket> m_replay;
		bool m_published = false;
	};

	// Notification is allocation-free/nonthrowing. It retires connection-owned
	// leases/sources only after the last admitted exchange finishes.
	explicit ConnectionRequests(std::function<void()> onClosed);
	~ConnectionRequests();
	ConnectionRequests(const ConnectionRequests&) = delete;
	ConnectionRequests& operator=(const ConnectionRequests&) = delete;
	Exchange Begin(const ControlPacket& request);
	bool Negotiated() const;
	std::uint16_t Minor() const;
	std::uint16_t ResponseMinor(std::uint16_t requested) const;
	bool Negotiate(std::uint16_t minor);
	void Close() noexcept;

private:
	enum class Phase
	{
		Idle,
		Executing,
		Closing,
		Closed
	};
	void Finish() noexcept;
	mutable std::mutex m_mutex;
	std::function<void()> m_onClosed;
	Phase m_phase = Phase::Idle;
	bool m_negotiated = false;
	std::uint16_t m_minor = 0;
	RequestId m_lastIdentity;
	std::vector<std::byte> m_lastPayload;
	std::optional<ControlPacket> m_lastResponse;
};
} // namespace simxr::control
