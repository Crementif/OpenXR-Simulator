#pragma once

#include "control/ProtocolTypes.h"
#include "presentation/FrameTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <openxr/openxr.h>
#include <span>
#include <vector>

namespace simxr
{
class Runtime;
class Session;
} // namespace simxr
namespace simxr::control
{
struct ControlSession
{
	ControlSessionId id;
	XrSession handle = XR_NULL_HANDLE;
	std::shared_ptr<Session> session;
};
class ControlSessions
{
  public:
	static constexpr std::size_t Capacity = 64;
	// Retains partial discovery on failure as well as successful resolution.
	// The service keeps this owner until request admission has reopened.
	class Snapshot
	{
		friend class ControlSessions;

	  public:
		std::span<const ControlSession> Sessions() const;
		std::span<const ControlSessionId> Ids() const;
		const ControlSession& Resolve(ControlSessionId session, SessionGeneration generation) const;

	  private:
		enum class Phase
		{
			Empty,
			Collecting,
			Ready
		};
		Phase m_phase = Phase::Empty;
		std::vector<ControlSession> m_sessions;
		std::array<ControlSessionId, Capacity> m_ids;
	};
	explicit ControlSessions(Runtime& runtime);
	void Discover(Snapshot& snapshot);

  private:
	struct Registration
	{
		ControlSessionId id;
		XrSession handle;
		std::weak_ptr<Session> session;
	};
	Runtime& m_runtime;
	std::mutex m_mutex;
	std::vector<Registration> m_sessions;
	std::uint64_t m_nextId = 1;
};
} // namespace simxr::control
