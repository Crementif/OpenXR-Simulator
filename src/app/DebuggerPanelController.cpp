#include "app/DebuggerPanelController.h"

#include <algorithm>
#include <utility>

namespace simxr::app
{
namespace wire = control;
namespace tag = wire::tag;
using namespace panel;
using Work = PanelWork;
using Clock = PanelClock;
DebuggerPanelController::DebuggerPanelController(RuntimePanelRequests& transport) : m_requests(transport)
{
	m_operations.reserve(128);
	m_effects.reserve(32);
}
const PanelSession& DebuggerPanelController::Selected() const noexcept
{
	return m_selected;
}
std::span<const PanelSession> DebuggerPanelController::Sessions() const noexcept
{
	return m_sessions;
}
std::span<const DebuggerOperation> DebuggerPanelController::Operations() const noexcept
{
	return m_operations;
}
const wire::Message& DebuggerPanelController::Status() const noexcept
{
	return m_status;
}
bool DebuggerPanelController::Manual() const noexcept
{
	return m_manual;
}
bool DebuggerPanelController::HasLease() const noexcept
{
	return m_lease != 0;
}
PanelScope DebuggerPanelController::Scope() const noexcept
{
	return {m_requests.Epoch(), m_selected.id, m_selected.generation};
}
PanelFields DebuggerPanelController::SessionFields() const
{
	return {wire::Field::U64(tag::Session, m_selected.id, true),
			wire::Field::U64(tag::Generation, m_selected.generation, true)};
}
std::string DebuggerPanelController::TakeError()
{
	return std::exchange(m_detail, {});
}
std::vector<DebuggerCaptureEvent> DebuggerPanelController::TakeCaptures()
{
	auto result = m_effects;
	m_effects.clear();
	return result;
}
bool DebuggerPanelController::Pending() const
{
	return m_requests.Has(Work::Acquire) || m_requests.Has(Work::Command) ||
		   std::any_of(m_operations.begin(), m_operations.end(),
					   [](const auto& operation)
					   {
						   return !Terminal(operation.phase);
					   });
}
void DebuggerPanelController::ResetEpoch()
{
	m_sessions.clear();
	m_selected = {};
	m_status = {};
	m_lease = 0;
	m_manual = false;
	m_operations.clear();
	m_effects.clear();
	m_deferred.reset();
	m_nextSessions = m_nextStatus = m_nextOperations = m_nextRenew = {};
}
void DebuggerPanelController::Relinquish()
{
	if (m_lease)
	{
		auto fields = SessionFields();
		fields.push_back(wire::Field::U64(tag::Lease, m_lease));
		m_requests.Send(wire::OpCode::Relinquish, Work::Release, std::move(fields));
	}
	m_lease = 0;
	m_manual = false;
	m_deferred.reset();
}
bool DebuggerPanelController::Relinquishing() const
{
	return m_requests.Has(Work::Release);
}
void DebuggerPanelController::ChooseSession(PanelSession session)
{
	Relinquish();
	m_selected = session;
	m_operations.clear();
	m_effects.clear();
	m_acknowledged = 0;
	m_status = {};
	m_nextStatus = {};
}
void DebuggerPanelController::Command(wire::CommandKind kind, std::uint64_t capture, bool raw, std::uint32_t layer,
									  std::uint32_t view)
{
	if (!m_selected.id || m_requests.Snapshot().state != RuntimeConnectionState::Connected)
	{
		m_detail = "Connect to a running session first";
		return;
	}
	if (!m_lease)
	{
		m_deferred = DeferredCommand{kind, capture, raw, layer, view};
		if (!m_requests.Has(Work::Acquire))
			m_requests.Send(wire::OpCode::Acquire, Work::Acquire, SessionFields());
		return;
	}
	if (m_operations.size() >= 128)
	{
		m_detail = "Wait for pending operations to settle before issuing more commands";
		return;
	}
	auto fields = SessionFields();
	fields.push_back(wire::Field::U64(tag::Lease, m_lease, true));
	fields.push_back(wire::Field::U64(tag::CommandId, m_command, true));
	fields.push_back(wire::Field::U32(tag::CommandKind, static_cast<std::uint32_t>(kind), true));
	if (kind == wire::CommandKind::Step || kind == wire::CommandKind::StrictAdmission)
	{
		fields.push_back(wire::Field::U32(tag::Lookahead, 1));
		fields.push_back(wire::Field::Bytes(
			tag::Target,
			wire::EncodeFields(PanelFields{
				wire::Field::U32(tag::CaptureKind, static_cast<std::uint32_t>(raw ? wire::TargetKind::LayerView
																				  : wire::TargetKind::ComposedEyes)),
				wire::Field::U32(tag::Layer, layer), wire::Field::U32(tag::View, view)})));
	}
	if (kind == wire::CommandKind::Step &&
		(U64(m_status, tag::Capabilities) & static_cast<std::uint64_t>(wire::Capability::SnapshotPins)))
		fields.push_back(wire::Field::Bool(tag::AdvanceDisplay, true));
	if (kind == wire::CommandKind::ConfigureSnapshots)
		fields.push_back(wire::Field::U32(tag::SnapshotBudgetMiB, static_cast<std::uint32_t>(capture), true));
	if (kind == wire::CommandKind::Cancel)
		fields.push_back(wire::Field::U64(tag::OperationId, capture, true));
	if (kind == wire::CommandKind::Release || kind == wire::CommandKind::PinSnapshot ||
		kind == wire::CommandKind::UnpinSnapshot)
		fields.push_back(wire::Field::U64(tag::CaptureId, capture, true));
	DebuggerOperation prepared;
	prepared.local = m_nextOperation++;
	prepared.command = m_command;
	prepared.kind = kind;
	prepared.subject = capture;
	const auto local = prepared.local;
	m_operations.push_back(std::move(prepared));
	try
	{
		if (m_requests.Send(wire::OpCode::Submit, Work::Command, std::move(fields), local))
			++m_command;
		else
			m_operations.pop_back();
	}
	catch (...)
	{
		m_operations.pop_back();
		throw;
	}
}
void DebuggerPanelController::CancelPending()
{
	const auto found = std::find_if(m_operations.rbegin(), m_operations.rend(),
									[](const auto& operation)
									{
										return operation.id && !Terminal(operation.phase) &&
											   operation.kind != wire::CommandKind::Cancel;
									});
	if (found != m_operations.rend())
		Command(wire::CommandKind::Cancel, found->id);
	else
		m_detail = "No accepted pending operation to cancel";
}
void DebuggerPanelController::InterruptSession()
{
	std::erase_if(m_sessions,
				  [&](const auto& session)
				  {
					  return session.id == m_selected.id && session.generation == m_selected.generation;
				  });
	m_selected = {};
	m_manual = false;
	m_lease = 0;
	m_deferred.reset();
	for (auto& operation : m_operations)
		if (!Terminal(operation.phase))
		{
			operation.phase = "interrupted";
			operation.progress = "session_closed";
			operation.polling = false;
		}
}
void DebuggerPanelController::Failed(const PanelTask& task, wire::ErrorCode error, const std::string& errorDetail)
{
	if (task.session && (error == wire::ErrorCode::NotFound || error == wire::ErrorCode::Closed ||
						 error == wire::ErrorCode::StaleGeneration))
	{
		InterruptSession();
		return;
	}
	for (auto& operation : m_operations)
		if ((task.kind == Work::Operation && operation.id == task.id) ||
			(task.kind == Work::Command && operation.local == task.id))
		{
			operation.polling = false;
			operation.phase = "failed";
			operation.progress = errorDetail;
		}
	if (error == wire::ErrorCode::LeaseRequired)
		m_lease = 0;
}
bool DebuggerPanelController::Handle(const PanelTask& task, const wire::Message& message)
{
	switch (task.kind)
	{
	case Work::Sessions:
	{
		std::vector<PanelSession> prepared;
		for (const auto& entry : Nested(message.Find(tag::Sessions)).fields)
		{
			const auto session = Nested(&entry);
			prepared.push_back(
				{U64(session, tag::Session), U64(session, tag::Generation), Boolean(session, tag::Running)});
		}
		m_sessions = std::move(prepared);
		if (std::none_of(m_sessions.begin(), m_sessions.end(),
						 [&](const auto& session)
						 {
							 return session.id == m_selected.id && session.generation == m_selected.generation;
						 }))
			ChooseSession(m_sessions.empty() ? PanelSession{} : m_sessions.front());
		break;
	}
	case Work::Status:
		m_status = message;
		m_manual = Boolean(message, tag::Manual);
		break;
	case Work::Acquire:
		m_lease = U64(message, tag::Lease);
		m_command = 1;
		m_acknowledged = 0;
		m_operations.clear();
		m_nextRenew = Clock::now() + std::chrono::seconds(5);
		if (m_deferred)
		{
			const auto request = *m_deferred;
			m_deferred.reset();
			Command(request.kind, request.capture, request.raw, request.layer, request.view);
		}
		break;
	case Work::Renew:
		break;
	case Work::Command:
		for (auto& operation : m_operations)
			if (operation.local == task.id)
				operation.id = U64(message, tag::OperationId);
		break;
	case Work::Acknowledge:
		m_acknowledged = task.id;
		break;
	case Work::Operation:
	{
		// The exchange has returned even if decoding or staging notices fails.
		// Keep the previous operation state retryable instead of stranding its
		// polling gate on a consumed reply.
		for (auto& operation : m_operations)
			if (operation.id == task.id)
				operation.polling = false;
		const auto phase = Text(message, tag::Phase);
		if (phase != "queued" && phase != "pending" && !Terminal(phase))
			throw std::runtime_error("Runtime returned an unsupported operation phase");
		for (auto& operation : m_operations)
			if (operation.id == task.id)
			{
				auto prepared = operation;
				prepared.polling = false;
				prepared.phase = phase;
				prepared.progress = Text(message, tag::Progress);
				std::vector<DebuggerCaptureEvent> pending;
				if (Terminal(phase))
				{
					if (phase == "complete" && (operation.kind == wire::CommandKind::PinSnapshot ||
						operation.kind == wire::CommandKind::UnpinSnapshot))
						pending.push_back({operation.subject, 0, false, true});
					if (operation.kind == wire::CommandKind::Release && prepared.progress == "released")
						pending.push_back({operation.subject, 0, true});
					const bool paused = operation.kind == wire::CommandKind::Pause;
					if (const auto capture = U64(message, tag::CaptureId))
						pending.push_back({capture, Frame(message), false, false, paused});
					for (const auto& capture : Nested(message.Find(tag::Captures)).fields)
						if (capture.AsU64())
							pending.push_back({*capture.AsU64(), Frame(message), false, false, paused});
				}
				if (pending.size() > 32 - m_effects.size())
					throw std::runtime_error("Runtime capture notices exceed the bounded catalog");
				operation = std::move(prepared);
				m_effects.insert(m_effects.end(), pending.begin(), pending.end());
			}
		break;
	}
	default:
		return false;
	}
	return true;
}
void DebuggerPanelController::Poll()
{
	const auto now = Clock::now();
	if (now >= m_nextSessions && !m_requests.Has(Work::Sessions))
	{
		m_requests.Send(wire::OpCode::Sessions, Work::Sessions);
		m_nextSessions = now + std::chrono::seconds(1);
	}
	if (m_selected.id && now >= m_nextStatus && !m_requests.Has(Work::Status))
	{
		m_requests.Send(wire::OpCode::Status, Work::Status, SessionFields());
		m_nextStatus = now + std::chrono::milliseconds(200);
	}
	if (m_lease && now >= m_nextRenew && !m_requests.Has(Work::Renew))
	{
		auto fields = SessionFields();
		fields.push_back(wire::Field::U64(tag::Lease, m_lease));
		m_requests.Send(wire::OpCode::Renew, Work::Renew, std::move(fields));
		m_nextRenew = now + std::chrono::seconds(5);
	}
	if (m_selected.id && now >= m_nextOperations)
	{
		for (auto& operation : m_operations)
			if (operation.id && !Terminal(operation.phase) && !operation.polling)
			{
				auto fields = SessionFields();
				fields.push_back(wire::Field::U64(tag::OperationId, operation.id, true));
				operation.polling =
					m_requests.Send(wire::OpCode::Operation, Work::Operation, std::move(fields), operation.id) != 0;
			}
		m_nextOperations = now + std::chrono::milliseconds(50);
	}
	if (m_lease && !m_requests.Has(Work::Acknowledge))
	{
		auto through = m_acknowledged;
		while (std::any_of(m_operations.begin(), m_operations.end(),
						   [&](const auto& operation)
						   {
							   return operation.command == through + 1 && Terminal(operation.phase);
						   }))
			++through;
		if (through > m_acknowledged)
		{
			auto fields = SessionFields();
			fields.push_back(wire::Field::U64(tag::Lease, m_lease));
			fields.push_back(wire::Field::U64(tag::CommandId, through));
			m_requests.Send(wire::OpCode::Acknowledge, Work::Acknowledge, std::move(fields), through);
		}
		if (m_operations.size() > 64)
			std::erase_if(m_operations,
						  [&](const auto& operation)
						  {
							  return operation.command <= m_acknowledged && operation.command + 64 < m_command;
						  });
	}
}
} // namespace simxr::app
