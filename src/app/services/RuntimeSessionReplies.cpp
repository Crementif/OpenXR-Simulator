#include "Pch.h"
#include "app/services/RuntimeSession.h"
#include "control/SimulationControl.h"
#include <stdexcept>

namespace simxr::app
{
namespace wire = control;
namespace tag = wire::tag;
using panel::Boolean;
using panel::Text;
using panel::U32;
using panel::U64;
using Work = PanelWork;
void RuntimeSession::Reply(const PanelReply& response)
{
	const auto& task = response.task;
	const auto& reply = response.response;
	const auto& message = reply.packet.message;
	const auto selected = m_debugger.Selected();
	if (task.session && (task.session != selected.id || task.generation != selected.generation))
	{
		if (task.kind == Work::Retain || task.kind == Work::Preview || task.kind == Work::PastPreview)
			m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId, U64(message, tag::ImageReaderId));
		if (task.kind == Work::Inspect)
			m_requests.Release(wire::OpCode::ReleaseInspection, tag::InspectionId, U64(message, tag::InspectionId));
		if (task.kind == Work::Acquire && U64(message, tag::Lease))
			m_requests.Release(wire::OpCode::Relinquish, tag::Lease, U64(message, tag::Lease), task.session,
							   task.generation);
		return;
	}
	const bool stepReply =
		(task.kind == Work::Command || task.kind == Work::Operation) &&
		std::any_of(m_debugger.Operations().begin(), m_debugger.Operations().end(),
					[&](const auto& operation)
					{
						return operation.kind == wire::CommandKind::Step &&
							   (task.kind == Work::Command ? operation.local : operation.id) == task.id;
					});
	const auto error = U32(message, tag::Error, static_cast<std::uint32_t>(wire::ErrorCode::Internal));
	if (!reply.error.empty() || error)
	{
		const auto failure =
			!reply.error.empty() ? reply.error : Text(message, tag::Detail, "Runtime rejected the operation");
		if (task.kind == Work::PastPreview)
		{
			if (error != static_cast<std::uint32_t>(wire::ErrorCode::Busy))
				m_unavailablePreview = task.id;
			if (error != static_cast<std::uint32_t>(wire::ErrorCode::Busy) &&
				error != static_cast<std::uint32_t>(wire::ErrorCode::NotFound))
				Notify("Frame image unavailable", failure);
			return;
		}
		if (task.kind == Work::Haptics)
		{
			m_haptics.Motors(Clock::now(), false);
			m_nextHaptics = Clock::now() + std::chrono::seconds(1);
			if (failure != m_hapticFailure)
				Notify("Live haptics unavailable", failure);
			m_hapticFailure = failure;
			return;
		}
		const auto previous = m_debugger.Scope();
		m_debugger.Failed(task, static_cast<wire::ErrorCode>(error), failure);
		if (stepReply || task.kind == Work::Acquire)
			m_stepRequested = false;
		if (previous != m_debugger.Scope())
			ScopeChanged();
		else
		{
			if (!(task.kind == Work::Preview &&
				  (error == static_cast<std::uint32_t>(wire::ErrorCode::Busy) ||
				   error == static_cast<std::uint32_t>(wire::ErrorCode::UnsupportedCommand))))
				Notify(task.kind == Work::Preview ? "Preview image unavailable" : "Runtime refused a request", failure);
			if (reply.error.empty() && error == static_cast<std::uint32_t>(wire::ErrorCode::NotFound) &&
				(task.kind == Work::Capture || task.kind == Work::Retain || task.kind == Work::Inspect))
				m_workspace.MarkUnavailable(task.id);
			m_workspace.SetScope(m_debugger.Scope(), false);
			m_preview.SetScope(m_debugger.Scope());
		}
		if (task.kind == Work::Share)
			m_preview.ShareFailed(task.id);
		m_exports.Failed(task, failure);
		return;
	}
	if (task.kind == Work::Haptics)
	{
		try
		{
			m_haptics.Observe(wire::DecodeHaptics(message), m_hapticsRequested, Clock::now());
			m_hapticFailure.clear();
		}
		catch (const std::exception& error)
		{
			m_haptics.Motors(Clock::now(), false);
			m_nextHaptics = Clock::now() + std::chrono::seconds(1);
			if (m_hapticFailure != error.what())
				Notify("Live haptics unavailable", error.what());
			m_hapticFailure = error.what();
		}
		return;
	}
	const auto previous = m_debugger.Scope();
	if (m_debugger.Handle(task, message))
	{
		if (previous != m_debugger.Scope())
			ScopeChanged();
		const bool stepRequested = m_stepRequested;
		for (const auto& effect : m_debugger.TakeCaptures())
		{
			const bool requested = effect.kind == wire::CommandKind::Step && stepRequested;
			if (effect.refresh && !m_requests.Has(Work::Capture, effect.capture))
			{
				auto fields = m_debugger.SessionFields();
				fields.push_back(wire::Field::U64(tag::CaptureId, effect.capture, true));
				m_requests.Send(wire::OpCode::Capture, Work::Capture, std::move(fields), effect.capture);
			}
			if (effect.released)
				m_workspace.MarkReleased(effect.capture);
			else if (m_workspace.Add(effect.capture, effect.frame))
			{
				// Pausing keeps the frame it stopped on.
				if (effect.kind == wire::CommandKind::Pause && Supports(wire::Capability::SnapshotPins))
					m_debugger.Command(wire::CommandKind::PinSnapshot, effect.capture);
				// Following while paused: the newest stepped frame is the live edge.
				if (effect.frame && m_autoSelect && m_time.following)
				{
					ShowCapture(effect.capture);
					const auto scope = m_debugger.Scope();
					m_stepped = SteppedFrame{{scope.session, scope.generation, effect.frame}, scope.epoch, requested};
				}
			}
		}
		if (stepReply && task.kind == Work::Operation && panel::Terminal(Text(message, tag::Phase)))
			m_stepRequested = false;
		return;
	}
	if (m_exports.Handle(task, message))
		return;
	if (m_workspace.Handle(task, message))
	{
		if (task.kind == Work::Capture)
		{
			if (Boolean(message, tag::ImageAvailable))
			{
				auto fields = m_debugger.SessionFields();
				fields.push_back(wire::Field::U64(tag::CaptureId, task.id, true));
				m_requests.Send(wire::OpCode::RetainImage, Work::Retain, std::move(fields), task.id);
			}
			else if (m_workspace.SelectedId() == task.id)
				m_preview.ClearSelection();
		}
		return;
	}
	switch (task.kind)
	{
	case Work::FrameHistory:
		if (auto skipped = m_history.Accept(message); !skipped.empty() && m_historyNotice != skipped)
		{
			m_historyNotice = skipped;
			Notify("Frame history page skipped", std::move(skipped));
		}
		break;
	case Work::PastPreview:
		if (!m_time.following && m_time.cursor.frame == task.id)
		{
			if (Boolean(message, tag::ImageAvailable))
				m_preview.Retained(message, false);
			else
				m_unavailablePreview = task.id;
		}
		else
			m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId, U64(message, tag::ImageReaderId));
		break;
	case Work::Retain:
	case Work::Preview:
		m_preview.Retained(message, task.kind == Work::Preview);
		break;
	case Work::Share:
		m_preview.Shared(task.id, reply);
		break;
	default:
		break;
	}
}
} // namespace simxr::app
