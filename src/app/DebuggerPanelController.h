#pragma once

#include "app/RuntimePanelRequests.h"

#include <optional>
#include <span>

namespace simxr::app
{
struct DebuggerOperation
{
	std::uint64_t id = 0, local = 0, command = 0;
	control::CommandKind kind = control::CommandKind::Pause;
	std::string phase = "queued", progress;
	bool polling = false;
	std::uint64_t subject = 0;
};
struct DebuggerCaptureEvent
{
	std::uint64_t capture = 0, frame = 0;
	bool released = false, refresh = false;
};
// Owns one selected session's lease, command identity and bounded operation
// ledger. Widgets observe const values and request transitions, never edit them.
class DebuggerPanelController
{
  public:
	explicit DebuggerPanelController(RuntimePanelRequests& transport);
	void ResetEpoch();
	void ChooseSession(PanelSession session);
	void Command(control::CommandKind kind, std::uint64_t capture = 0, bool raw = false, std::uint32_t layer = 0,
				 std::uint32_t view = 0);
	void CancelPending();
	void Poll();
	bool Handle(const PanelTask& task, const control::Message& message);
	void Failed(const PanelTask& task, control::ErrorCode error, const std::string& errorDetail);
	std::vector<DebuggerCaptureEvent> TakeCaptures();
	std::string TakeError();
	const PanelSession& Selected() const noexcept;
	std::span<const PanelSession> Sessions() const noexcept;
	std::span<const DebuggerOperation> Operations() const noexcept;
	const control::Message& Status() const noexcept;
	PanelFields SessionFields() const;
	PanelScope Scope() const noexcept;
	bool Manual() const noexcept;
	bool HasLease() const noexcept;
	bool Pending() const;

  private:
	struct DeferredCommand
	{
		control::CommandKind kind;
		std::uint64_t capture;
		bool raw;
		std::uint32_t layer;
		std::uint32_t view;
	};
	void InterruptSession();
	RuntimePanelRequests& m_requests;
	std::vector<PanelSession> m_sessions;
	PanelSession m_selected;
	control::Message m_status;
	std::vector<DebuggerOperation> m_operations;
	std::vector<DebuggerCaptureEvent> m_effects;
	std::optional<DeferredCommand> m_deferred;
	std::uint64_t m_lease = 0, m_command = 1, m_nextOperation = 1, m_acknowledged = 0;
	bool m_manual = false;
	PanelClock::time_point m_nextSessions{}, m_nextStatus{}, m_nextOperations{}, m_nextRenew{};
	std::string m_detail;
};
} // namespace simxr::app
