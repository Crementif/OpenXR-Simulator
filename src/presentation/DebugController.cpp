#include "presentation/DebugController.h"
#include "Pch.h"
#include "graphics/SnapshotBudget.h"
#include "runtime/Session.h"

#include <limits>
#include <new>
#include <stdexcept>

namespace simxr
{
static bool Active(const DebugOperation& operation)
{
	return operation.phase == DebugPhase::Queued || operation.phase == DebugPhase::Pending;
}
static bool Admission(DebugCommand command)
{
	return command == DebugCommand::Step || command == DebugCommand::StrictAdmission;
}
static bool Settled(SnapshotPhase phase)
{
	return phase != SnapshotPhase::Reserved && phase != SnapshotPhase::Issued && phase != SnapshotPhase::Begun &&
		   phase != SnapshotPhase::GpuPending;
}
static bool After(const SnapshotRecord& record, const std::optional<FrameKey>& cursor)
{
	return !record.frame || !cursor || *record.frame > *cursor;
}

DebugController::DebugController(std::shared_ptr<Session> session, std::shared_ptr<Clock> clock,
								 DebugControllerConfig config)
	: m_session(std::move(session)), m_clock(std::move(clock)), m_config(config)
{
	if (!m_session || !m_clock || !config.ledgerCapacity || !config.maxLookahead || config.maxLookahead > 32 ||
		config.leaseDuration <= Duration::zero() || config.pollInterval <= Duration::zero())
		throw std::invalid_argument("Invalid debug controller configuration");
	m_owned.reserve(std::max(SnapshotBudget::MaxCapacity, m_session->Captures().Capacity()));
	m_cursors.reserve(std::max(SnapshotBudget::MaxCapacity, m_session->Captures().Capacity()) + 1);
	if (config.startWorker)
		m_worker = std::thread(&DebugController::Run, this);
}

DebugController::~DebugController()
{
	Close();
}

void DebugController::Close(bool resume) noexcept
{
	std::lock_guard lifecycle(m_lifecycle);
	{
		std::lock_guard lock(m_state);
		if (m_stopping)
			return;
		m_stopping = true;
	}
	m_changed.notify_all();
	if (m_worker.joinable())
		m_worker.join();
	std::lock_guard execute(m_execute);
	m_session->CancelCaptureReservations();
	{
		std::lock_guard lock(m_state);
		for (auto& operation : m_operations)
			if (Active(operation))
			{
				operation.phase = DebugPhase::Interrupted;
				operation.progress = DebugProgress::SessionEnded;
			}
		m_lease.reset();
	}
	try
	{
		if (resume)
			ResumeSession();
	}
	catch (...)
	{
	}
	RetireOwned();
	m_manual = false;
	m_boundaryCaptured = false;
	std::lock_guard lock(m_state);
	m_publishedManual = false;
}

std::optional<DebugLease> DebugController::Acquire(SessionGeneration expected)
{
	if (expected.value)
	{
		std::lock_guard execute(m_execute);
		std::optional<DebugLease> result;
		m_session->WithDebugGeneration(
			expected,
			[&]
			{
				result = Acquire();
			},
			false);
		return result;
	}
	const auto now = m_clock->Now();
	if (now > HostTime::max() - m_config.leaseDuration)
		return {};
	std::lock_guard lock(m_state);
	if (m_stopping || m_recovering || m_lease || m_nextLease == std::numeric_limits<std::uint64_t>::max())
		return {};
	m_lease = DebugLease{{m_nextLease++}, now + m_config.leaseDuration};
	m_retiredFloor = {};
	return m_lease;
}

bool DebugController::Renew(DebugLeaseId lease, SessionGeneration expected)
{
	if (expected.value)
	{
		std::lock_guard execute(m_execute);
		bool result = false;
		m_session->WithDebugGeneration(
			expected,
			[&]
			{
				result = Renew(lease);
			},
			false);
		return result;
	}
	const auto now = m_clock->Now();
	if (now > HostTime::max() - m_config.leaseDuration)
		return false;
	std::lock_guard lock(m_state);
	if (m_stopping || !m_lease || m_lease->id != lease || now >= m_lease->expires)
		return false;
	m_lease->expires = now + m_config.leaseDuration;
	return true;
}

bool DebugController::Relinquish(DebugLeaseId lease, SessionGeneration expected)
{
	if (expected.value)
	{
		std::lock_guard execute(m_execute);
		bool result = false;
		m_session->WithDebugGeneration(
			expected,
			[&]
			{
				result = Relinquish(lease);
			},
			false);
		return result;
	}
	std::lock_guard lock(m_state);
	if (m_stopping || !m_lease || m_lease->id != lease)
		return false;
	m_lease->expires = HostTime::min();
	m_changed.notify_all();
	return true;
}

DebugSubmission DebugController::Submit(DebugLeaseId lease, const DebugRequest& request)
{
	const auto now = m_clock->Now();
	std::lock_guard lock(m_state);
	if (m_stopping)
		return {DebugAdmission::Closed, {}};
	const auto previous = std::find_if(m_operations.begin(), m_operations.end(),
									   [&](const auto& operation)
									   {
										   return operation.lease == lease && operation.request.id == request.id;
									   });
	if (previous != m_operations.end())
		return {previous->request == request ? DebugAdmission::Duplicate : DebugAdmission::Conflict, previous->id};
	if (!m_lease || m_lease->id != lease || now >= m_lease->expires || m_recovering)
		return {DebugAdmission::LeaseRequired, {}};
	if (request.id <= m_retiredFloor)
		return {DebugAdmission::Retired, {}};
	if ((request.command == DebugCommand::ConfigureSnapshots &&
		 (request.snapshotBudgetMiB < SnapshotBudget::MinMiB || request.snapshotBudgetMiB > SnapshotBudget::MaxMiB)) ||
		!request.id.value || request.command > DebugCommand::ConfigureSnapshots || !request.lookahead ||
		request.lookahead > m_config.maxLookahead || !request.recordingCapacity || request.recordingCapacity > 4 ||
		(request.command == DebugCommand::Cancel && !request.operation.value) ||
		((request.command == DebugCommand::Release || request.command == DebugCommand::PinSnapshot ||
		  request.command == DebugCommand::UnpinSnapshot) &&
		 !request.capture.value))
		return {DebugAdmission::Invalid, {}};
	if (!request.additionalTargets.empty())
	{
		if (request.command != DebugCommand::Step || request.additionalTargets.size() > 7 ||
			request.target.kind == CaptureKind::VirtualDisplay)
			return {DebugAdmission::Invalid, {}};
		for (std::size_t index = 0; index < request.additionalTargets.size(); ++index)
		{
			const auto target = request.additionalTargets[index];
			if (target == request.target || target.kind == CaptureKind::VirtualDisplay ||
				std::find(request.additionalTargets.begin(), request.additionalTargets.begin() + index, target) !=
					request.additionalTargets.begin() + index)
				return {DebugAdmission::Invalid, {}};
		}
	}
	if (m_operations.size() >= m_config.ledgerCapacity || m_nextOperation == std::numeric_limits<std::uint64_t>::max())
		return {DebugAdmission::Capacity, {}};
	DebugOperation operation;
	operation.id = {m_nextOperation};
	operation.lease = lease;
	try
	{
		operation.request = request;
		m_operations.push_back(std::move(operation));
	}
	catch (const std::bad_alloc&)
	{
		return {DebugAdmission::Capacity, {}};
	}
	++m_nextOperation;
	m_changed.notify_all();
	return {DebugAdmission::Accepted, m_operations.back().id};
}

bool DebugController::Acknowledge(DebugLeaseId lease, DebugCommandId through, std::vector<DebugOperation>* output,
								  SessionGeneration expected)
{
	if (output && !output->empty())
		return false;
	std::vector<DebugOperation> retired;
	bool accepted = false;
	{
		std::lock_guard execute(m_execute);
		const auto mutate = [&]
		{
			std::lock_guard lock(m_state);
			if (!lease.value || !through.value ||
				std::any_of(m_operations.begin(), m_operations.end(),
							[&](const auto& operation)
							{
								return operation.lease == lease && operation.request.id <= through && Active(operation);
							}))
				return false;
			const bool current = m_lease && m_lease->id == lease;
			if (!current && std::none_of(m_operations.begin(), m_operations.end(),
										 [&](const auto& operation)
										 {
											 return operation.lease == lease;
										 }))
				return false;
			retired.reserve(m_operations.size());
			for (auto& operation : m_operations)
				if (operation.lease == lease && operation.request.id <= through)
					retired.push_back(std::move(operation));
			std::erase_if(m_operations,
						  [&](const auto& operation)
						  {
							  return operation.lease == lease && operation.request.id <= through;
						  });
			if (current)
				m_retiredFloor = std::max(m_retiredFloor, through);
			return true;
		};
		if (expected.value)
			m_session->WithDebugGeneration(
				expected,
				[&]
				{
					accepted = mutate();
				},
				false);
		else
			accepted = mutate();
	}
	// Last native output readers may reenter status services on destruction.
	// Transfer/destroy them only after execution, lifecycle and state unlock.
	if (output && accepted)
		*output = std::move(retired);
	return accepted;
}

std::optional<DebugOperation> DebugController::Read(DebugOperationId operation) const
{
	std::lock_guard lock(m_state);
	const auto entry = std::find_if(m_operations.begin(), m_operations.end(),
									[&](const auto& value)
									{
										return value.id == operation;
									});
	return entry == m_operations.end() ? std::nullopt : std::optional{*entry};
}

DebugControllerSnapshot DebugController::Snapshot() const
{
	DebugControllerSnapshot snapshot;
	{
		std::lock_guard lock(m_state);
		snapshot.lease = m_lease;
		snapshot.manual = m_publishedManual;
		snapshot.inspected = m_publishedCursor;
		snapshot.inspectedTarget = m_publishedTarget;
		snapshot.commands = m_operations.size();
	}
	snapshot.maxLookahead = m_config.maxLookahead;
	snapshot.composedEyes = m_session->SupportsCapture();
	snapshot.layerViews = m_session->SupportsCapture({CaptureKind::LayerView});
	snapshot.depthViews = m_session->SupportsCapture({CaptureKind::LayerDepth});
	snapshot.filteredComposition = m_session->SupportsCapture({CaptureKind::ProjectionLayers});
	snapshot.pipeline = m_session->Scheduler().Snapshot();
	snapshot.retention = m_session->Captures().Snapshot();
	snapshot.imageRecording = m_session->ImageRecording();
	return snapshot;
}

void DebugController::Run() noexcept
{
	for (;;)
	{
		Poll();
		std::unique_lock lock(m_state);
		if (m_stopping)
			return;
		m_changed.wait_for(lock, m_config.pollInterval);
		if (m_stopping)
			return;
	}
}

void DebugController::Publish(DebugOperation operation)
{
	std::lock_guard lock(m_state);
	const auto entry = std::find_if(m_operations.begin(), m_operations.end(),
									[&](const auto& value)
									{
										return value.id == operation.id;
									});
	if (entry != m_operations.end())
		*entry = std::move(operation);
}

void DebugController::Interrupt(DebugLeaseId lease, DebugProgress reason, std::optional<DebugOperationId> except)
{
	std::lock_guard lock(m_state);
	for (auto& operation : m_operations)
		if (operation.lease == lease && (!except || operation.id < *except) && Active(operation))
		{
			operation.phase = DebugPhase::Interrupted;
			operation.progress = reason;
		}
}

bool DebugController::ResumeSession()
{
	if (!m_manual)
		return true;
	m_session->CancelCaptureReservations();
	const auto state = m_session->Snapshot();
	if (state.running && state.generation == m_generation)
	{
		const auto boundary =
			std::max(m_session->Scheduler().Snapshot().nextAdmission, m_session->Display().NextTick());
		const auto anchor = m_clock->Now();
		if (!m_session->Display().Resume(anchor, boundary) ||
			XR_FAILED(m_session->Scheduler().Resume(anchor, boundary)))
			return false;
	}
	m_manual = false;
	m_boundaryCaptured = false;
	return true;
}

void DebugController::Poll() noexcept
{
	std::lock_guard execute(m_execute);
	{
		std::lock_guard lock(m_state);
		if (m_stopping)
			return;
	}
	try
	{
		m_session->Captures().Poll();
		std::erase_if(m_owned,
					  [&](const auto& owned)
					  {
						  return !m_session->Captures().Read(owned.capture);
					  });
		std::optional<DebugLeaseId> expired;
		bool recovering;
		std::size_t count;
		{
			const auto now = m_clock->Now();
			std::lock_guard lock(m_state);
			if (m_lease && now >= m_lease->expires)
			{
				expired = m_lease->id;
				m_lease.reset();
				m_recovering = true;
			}
			count = m_operations.size();
			recovering = m_recovering;
		}
		if (expired)
		{
			Interrupt(*expired, DebugProgress::LeaseExpired);
			m_session->CancelCaptureReservations();
			RetireOwned(*expired);
		}
		if (recovering)
		{
			ResumeSession();
			std::lock_guard lock(m_state);
			m_recovering = m_manual;
		}
		const auto session = m_session->Snapshot();
		if (m_manual && (!session.running || session.generation != m_generation))
		{
			std::lock_guard lock(m_state);
			for (auto& operation : m_operations)
				if (Active(operation))
				{
					operation.phase = DebugPhase::Interrupted;
					operation.progress = DebugProgress::SessionEnded;
				}
			m_manual = false;
			m_boundaryCaptured = false;
			m_recovering = false;
		}
		for (std::size_t index = 0; index < count; ++index)
		{
			DebugOperation operation;
			{
				std::lock_guard lock(m_state);
				if (!Active(m_operations[index]))
					continue;
				operation = m_operations[index];
			}
			try
			{
				if (operation.request.generation.value)
				{
					if (!m_session->WithDebugGeneration(operation.request.generation,
														[&]
														{
															Process(operation);
														}))
						Fail(operation, DebugProgress::SessionEnded, XR_ERROR_SESSION_NOT_RUNNING);
				}
				else
					Process(operation);
			}
			catch (const std::bad_alloc&)
			{
				if (Admission(operation.request.command))
					m_session->CancelCaptureReservations();
				Fail(operation, DebugProgress::RetentionFailure, XR_ERROR_OUT_OF_MEMORY);
			}
			catch (...)
			{
				if (Admission(operation.request.command))
					m_session->CancelCaptureReservations();
				Fail(operation, DebugProgress::SessionEnded, XR_ERROR_RUNTIME_FAILURE);
			}
			Publish(std::move(operation));
		}
	}
	catch (...)
	{
		// Polling failures never imply GPU completion. A later poll or teardown
		// can still recover; control/status access does not take execute.
	}
	std::lock_guard lock(m_state);
	m_publishedManual = m_manual;
	m_publishedCursor = m_cursor;
	m_publishedTarget = m_cursorTarget;
}

void DebugController::Fail(DebugOperation& operation, DebugProgress progress, XrResult result)
{
	operation.phase = DebugPhase::Failed;
	operation.progress = progress;
	operation.result = result;
}

bool DebugController::OtherAdmission(DebugOperationId current) const
{
	std::lock_guard lock(m_state);
	return std::any_of(m_operations.begin(), m_operations.end(),
					   [&](const auto& value)
					   {
						   return value.id != current && value.phase == DebugPhase::Pending &&
								  Admission(value.request.command);
					   });
}

std::vector<SnapshotRecord> DebugController::OrderedCaptures(CaptureTarget target) const
{
	std::optional<FrameKey> cursor;
	for (const auto& value : m_cursors)
		if (value.target == target)
			cursor = value.frame;
	std::vector<SnapshotRecord> records;
	for (const auto id : m_session->Captures().Snapshot().captures)
		if (auto record = m_session->Captures().Read(id);
			record && record->generation == m_generation && record->target == target && After(*record, cursor))
			records.push_back(std::move(*record));
	std::sort(records.begin(), records.end(),
			  [](const auto& left, const auto& right)
			  {
				  if (left.frame && right.frame)
					  return *left.frame < *right.frame;
				  if (left.frame != right.frame)
					  return left.frame.has_value();
				  return left.id < right.id;
			  });
	return records;
}

std::vector<SnapshotRecord> DebugController::Candidates(DebugOperation& operation) const
{
	auto records = OrderedCaptures(operation.request.target);
	if (!operation.request.additionalTargets.empty())
		std::erase_if(records,
					  [&](const auto& record)
					  {
						  return !record.frame ||
								 std::any_of(operation.request.additionalTargets.begin(),
											 operation.request.additionalTargets.end(),
											 [&](const auto target)
											 {
												 return !m_session->Captures().Find(*record.frame, target);
											 });
					  });
	std::erase_if(records,
				  [&](const auto& record)
				  {
					  if (record.phase != SnapshotPhase::Discarded && record.phase != SnapshotPhase::Cancelled)
						  return false;
					  if (std::find(operation.diagnostics.begin(), operation.diagnostics.end(), record.id) ==
						  operation.diagnostics.end())
						  operation.diagnostics.push_back(record.id);
					  return true;
				  });
	return records;
}

void DebugController::Own(DebugLeaseId lease, CaptureId capture)
{
	std::erase_if(m_owned,
				  [&](const auto& owned)
				  {
					  return !m_session->Captures().Read(owned.capture);
				  });
	if (std::none_of(m_owned.begin(), m_owned.end(),
					 [&](const auto& owned)
					 {
						 return owned.capture == capture;
					 }))
	{
		if (m_owned.size() == m_owned.capacity())
			throw std::logic_error("Capture ownership exceeded snapshot capacity");
		m_owned.push_back({lease, capture});
	}
}

void DebugController::RetireOwned(std::optional<DebugLeaseId> lease)
{
	if (m_recordingLease && (!lease || *m_recordingLease == *lease))
	{
		m_session->StopImageRecording();
		m_recordingLease.reset();
	}
	for (const auto& owned : m_owned)
		if (!lease || owned.lease == *lease)
		{
			m_session->Captures().Release(owned.capture);
			m_session->RefillImageRecording(owned.capture);
		}
	std::erase_if(m_owned,
				  [&](const auto& owned)
				  {
					  return !lease || owned.lease == *lease;
				  });
	for (std::size_t index = 0;; ++index)
	{
		std::shared_ptr<const FrameOutput> retired;
		{
			std::lock_guard lock(m_state);
			if (index == m_operations.size())
				break;
			auto& operation = m_operations[index];
			if ((!lease || operation.lease == *lease) && operation.display)
				retired = std::move(operation.display->output);
		}
	}
}

void DebugController::Inspect(DebugOperation& operation, const SnapshotRecord& record)
{
	operation.frame = record.frame;
	operation.capture = record.id;
	operation.result = record.phase == SnapshotPhase::Unavailable && XR_SUCCEEDED(record.result)
						   ? XR_ERROR_FEATURE_UNSUPPORTED
						   : record.result;
	if (!Settled(record.phase))
		return;
	if (!operation.request.additionalTargets.empty())
	{
		std::vector<CaptureId> bundle{record.id};
		for (const auto target : operation.request.additionalTargets)
		{
			const auto id = m_session->Captures().Find(*record.frame, target);
			const auto member = id ? m_session->Captures().Read(*id) : std::nullopt;
			if (!member || !Settled(member->phase))
				return;
			bundle.push_back(*id);
			if (member->phase == SnapshotPhase::Failed || member->phase == SnapshotPhase::Unavailable)
				operation.result = XR_FAILED(member->result) ? member->result : XR_ERROR_FEATURE_UNSUPPORTED;
		}
		operation.captures = std::move(bundle);
	}
	operation.phase = XR_FAILED(operation.result) || record.phase == SnapshotPhase::Failed ||
							  record.phase == SnapshotPhase::Unavailable
						  ? DebugPhase::Failed
						  : DebugPhase::Complete;
	operation.progress = record.phase == SnapshotPhase::Ready		? DebugProgress::SnapshotReady
						 : record.phase == SnapshotPhase::Clear		? DebugProgress::Clear
						 : record.phase == SnapshotPhase::Empty		? DebugProgress::Empty
						 : record.phase == SnapshotPhase::Discarded ? DebugProgress::Discarded
						 : record.phase == SnapshotPhase::Cancelled ? DebugProgress::Cancelled
																	: DebugProgress::RetentionFailure;
	if (operation.phase == DebugPhase::Failed)
		operation.progress = DebugProgress::RetentionFailure;
	if (operation.request.advanceDisplay && operation.request.command == DebugCommand::Step && record.metadata)
		m_session->Display().Advance(record.metadata->prediction.displayTime);
	m_cursor = record.frame;
	m_cursorTarget = record.target;
	// Raw requests only target future submissions. Once their retained records
	// are gone, no historical composed image can recreate an old raw cursor.
	const auto retained = m_session->Captures().Snapshot().captures;
	std::erase_if(m_cursors,
				  [&](const auto& cursor)
				  {
					  return cursor.target.IsLayerImage() &&
							 std::none_of(retained.begin(), retained.end(),
										  [&](auto id)
										  {
											  const auto value = m_session->Captures().Read(id);
											  return value && value->generation == m_generation &&
													 value->target == cursor.target;
										  });
				  });
	if (record.frame)
	{
		auto cursor = std::find_if(m_cursors.begin(), m_cursors.end(),
								   [&](const auto& value)
								   {
									   return value.target == record.target;
								   });
		if (cursor != m_cursors.end())
			cursor->frame = *record.frame;
		else
		{
			if (m_cursors.size() == m_cursors.capacity())
				throw std::logic_error("Target cursors exceeded snapshot capacity");
			m_cursors.push_back({record.target, *record.frame});
		}
	}
	m_session->CancelCaptureReservations();
}

void DebugController::Settle(DebugOperation& operation)
{
	operation.outstanding.clear();
	if (operation.request.command == DebugCommand::Pause)
	{
		bool settled = true;
		for (const auto id : operation.captures)
		{
			const auto record = m_session->Captures().Read(id);
			if (!record)
			{
				Fail(operation, DebugProgress::RetentionFailure, XR_ERROR_RUNTIME_FAILURE);
				return;
			}
			if (record->phase == SnapshotPhase::Failed || record->phase == SnapshotPhase::Unavailable)
			{
				if (record->frame)
					operation.outstanding.push_back(*record->frame);
				Fail(operation, DebugProgress::RetentionFailure,
					 XR_FAILED(record->result) ? record->result : XR_ERROR_RUNTIME_FAILURE);
				return;
			}
			if (!Settled(record->phase))
			{
				settled = false;
				if (record->frame)
					operation.outstanding.push_back(*record->frame);
			}
		}
		const auto pipeline = m_session->Scheduler().Snapshot();
		for (const auto& frame : {pipeline.waited, pipeline.begun})
			if (frame)
			{
				settled = false;
				const FrameKey key{m_generation, frame->id};
				if (std::find(operation.outstanding.begin(), operation.outstanding.end(), key) ==
					operation.outstanding.end())
					operation.outstanding.push_back(key);
			}
		operation.phase = settled ? DebugPhase::Complete : DebugPhase::Pending;
		operation.progress = settled ? DebugProgress::Paused : DebugProgress::Pausing;
		return;
	}
	if (operation.request.command == DebugCommand::StrictAdmission)
	{
		const auto record = m_session->Captures().Read(operation.captures.front());
		if (!record)
		{
			operation.phase = DebugPhase::Cancelled;
			operation.progress = DebugProgress::Cancelled;
		}
		else if (record->frame)
		{
			operation.phase = DebugPhase::Complete;
			operation.progress = DebugProgress::Issued;
			operation.frame = record->frame;
			operation.capture = record->id;
		}
		return;
	}
	if (operation.request.command == DebugCommand::Step)
	{
		const auto records = Candidates(operation);
		if (!records.empty() && records.front().frame)
		{
			const auto& record = records.front();
			Inspect(operation, record);
			if (!Active(operation))
				return;
			operation.outstanding.push_back(*record.frame);
			operation.progress = record.phase == SnapshotPhase::GpuPending ? DebugProgress::GpuPending
								 : record.phase == SnapshotPhase::Begun	   ? DebugProgress::Begun
																		   : DebugProgress::Issued;
		}
		else
			operation.progress = DebugProgress::WaitingSubmission;
		const auto pipeline = m_session->Scheduler().Snapshot();
		if (pipeline.waitPending && !pipeline.permits && operation.progress != DebugProgress::GpuPending)
			operation.progress = DebugProgress::PipelineStall;
	}
}

void DebugController::Process(DebugOperation& operation)
{
	if (operation.phase == DebugPhase::Pending)
	{
		Settle(operation);
		return;
	}
	const auto command = operation.request.command;
	if ((command == DebugCommand::Pause && operation.request.target != CaptureTarget{}) ||
		(Admission(command) && !m_session->SupportsCapture(operation.request.target)))
	{
		operation.retention = SnapshotAdmission::Unsupported;
		Fail(operation, DebugProgress::Unsupported, XR_ERROR_FEATURE_UNSUPPORTED);
		return;
	}
	if (command == DebugCommand::StartImageRecording)
	{
		if (m_manual)
		{
			Fail(operation, DebugProgress::Busy, XR_ERROR_CALL_ORDER_INVALID);
			return;
		}
		operation.captures.resize(operation.request.recordingCapacity);
		operation.retention =
			m_session->StartImageRecording(operation.request.target, operation.request.recordingCapacity,
										   operation.request.recordingFrames, operation.captures);
		if (operation.retention != SnapshotAdmission::Accepted)
		{
			operation.captures.clear();
			Fail(operation,
				 operation.retention == SnapshotAdmission::Unsupported ? DebugProgress::Unsupported
																	   : DebugProgress::RetentionFailure,
				 XR_ERROR_LIMIT_REACHED);
			return;
		}
		m_recordingLease = operation.lease;
		for (const auto capture : operation.captures)
			Own(operation.lease, capture);
		operation.phase = DebugPhase::Complete;
		operation.progress = DebugProgress::RecordingStarted;
		return;
	}
	if (command == DebugCommand::StopImageRecording)
	{
		m_session->StopImageRecording();
		m_recordingLease.reset();
		operation.phase = DebugPhase::Complete;
		operation.progress = DebugProgress::RecordingStopped;
		return;
	}
	if (command == DebugCommand::Resume)
	{
		m_session->StopImageRecording();
		m_recordingLease.reset();
		Interrupt(operation.lease, DebugProgress::Resumed, operation.id);
		if (!ResumeSession())
			Fail(operation, DebugProgress::SessionEnded, XR_ERROR_RUNTIME_FAILURE);
		else
		{
			operation.phase = DebugPhase::Complete;
			operation.progress = DebugProgress::Resumed;
		}
		return;
	}
	if (command == DebugCommand::ConfigureSnapshots)
	{
		SnapshotBudget::Configure(operation.request.snapshotBudgetMiB);
		operation.phase = DebugPhase::Complete;
		operation.progress = DebugProgress::SnapshotReady;
		return;
	}
	if (command == DebugCommand::PinSnapshot || command == DebugCommand::UnpinSnapshot)
	{
		if (!m_session->Captures().SetPinned(operation.request.capture, command == DebugCommand::PinSnapshot))
			Fail(operation, DebugProgress::RetentionFailure, XR_ERROR_HANDLE_INVALID);
		else
		{
			operation.capture = operation.request.capture;
			operation.phase = DebugPhase::Complete;
			operation.progress = DebugProgress::SnapshotReady;
		}
		return;
	}
	if (command == DebugCommand::Release)
	{
		operation.capture = operation.request.capture;
		const auto capture = m_session->Captures().Read(operation.request.capture);
		if (capture && capture->phase == SnapshotPhase::Reserved)
		{
			Fail(operation, DebugProgress::Busy, XR_ERROR_CALL_ORDER_INVALID);
			return;
		}
		bool interrupted = false;
		{
			std::lock_guard lock(m_state);
			for (auto& active : m_operations)
				if (Active(active) && Admission(active.request.command) &&
					(active.capture == operation.request.capture ||
					 std::find(active.captures.begin(), active.captures.end(), operation.request.capture) !=
						 active.captures.end()))
				{
					active.phase = DebugPhase::Cancelled;
					active.progress = DebugProgress::Cancelled;
					interrupted = true;
				}
		}
		if (interrupted)
			m_session->CancelCaptureReservations();
		if (!m_session->Captures().Release(operation.request.capture))
			Fail(operation, DebugProgress::RetentionFailure, XR_ERROR_HANDLE_INVALID);
		else
		{
			operation.phase = DebugPhase::Complete;
			operation.progress = DebugProgress::Released;
			std::erase_if(m_owned,
						  [&](const auto& owned)
						  {
							  return owned.capture == operation.request.capture;
						  });
			if (const auto replacement = m_session->RefillImageRecording(operation.request.capture); replacement.value)
				Own(operation.lease, replacement);
		}
		return;
	}
	if (command == DebugCommand::Cancel)
	{
		bool cancelled = false;
		{
			std::lock_guard lock(m_state);
			for (auto& target : m_operations)
				if (target.id == operation.request.operation && target.lease == operation.lease && Active(target))
				{
					target.phase = DebugPhase::Cancelled;
					target.progress = DebugProgress::Cancelled;
					cancelled = true;
				}
		}
		if (cancelled)
			m_session->CancelCaptureReservations();
		operation.phase = DebugPhase::Complete;
		operation.progress = DebugProgress::Cancelled;
		return;
	}
	if (!m_session->Snapshot().running)
	{
		Fail(operation, DebugProgress::SessionEnded, XR_ERROR_SESSION_NOT_RUNNING);
		return;
	}
	if (command == DebugCommand::Pause)
	{
		m_session->StopImageRecording();
		m_recordingLease.reset();
		m_session->Scheduler().Pause();
		m_session->CancelCaptureReservations();
		Interrupt(operation.lease, DebugProgress::Pausing, operation.id);
		m_manual = true;
		const auto generation = m_session->Snapshot().generation;
		if (generation != m_generation)
		{
			m_cursor.reset();
			m_cursors.clear();
		}
		m_generation = generation;
		if (!m_session->Display().Pause() ||
			XR_FAILED(m_session->Scheduler().RebasePaused(
				std::max(m_session->Scheduler().Snapshot().nextAdmission, m_session->Display().NextTick()))))
		{
			Fail(operation, DebugProgress::SessionEnded, XR_ERROR_RUNTIME_FAILURE);
			return;
		}
		const auto previous = m_session->Captures().Snapshot().captures;
		const auto boundary = m_session->CaptureBoundary();
		operation.retention = boundary.result;
		if (boundary.result != SnapshotAdmission::Accepted)
		{
			Fail(operation,
				 boundary.result == SnapshotAdmission::Unsupported ? DebugProgress::Unsupported
																   : DebugProgress::RetentionFailure,
				 XR_ERROR_LIMIT_REACHED);
			return;
		}
		for (const auto capture : boundary.captures)
			if (std::find(previous.begin(), previous.end(), capture) == previous.end())
				Own(operation.lease, capture);
		m_boundaryCaptured = true;
		operation.captures = boundary.captures;
		operation.phase = DebugPhase::Pending;
		Settle(operation);
		return;
	}
	if (!m_manual || !m_boundaryCaptured)
	{
		Fail(operation, DebugProgress::Pausing, XR_ERROR_CALL_ORDER_INVALID);
		return;
	}
	if (command == DebugCommand::DisplayStep)
	{
		if (operation.request.target != CaptureTarget{} &&
			operation.request.target != CaptureTarget{CaptureKind::VirtualDisplay})
		{
			Fail(operation, DebugProgress::Unsupported, XR_ERROR_FEATURE_UNSUPPORTED);
			return;
		}
		const auto capture = m_session->Captures().Reserve({CaptureKind::VirtualDisplay});
		operation.retention = capture.result;
		if (capture.result != SnapshotAdmission::Accepted)
		{
			Fail(operation, DebugProgress::RetentionFailure, XR_ERROR_LIMIT_REACHED);
			return;
		}
		const auto release = [&]
		{
			m_session->Captures().Release(capture.id);
			std::erase_if(m_owned,
						  [&](const auto& owned)
						  {
							  return owned.capture == capture.id;
						  });
		};
		std::optional<PresentationTick> tick;
		try
		{
			Own(operation.lease, capture.id);
			tick = m_session->Display().Step();
			if (!tick || !m_session->Captures().PublishDisplay(capture.id, *tick, tick->metadata))
			{
				release();
				Fail(operation, DebugProgress::SessionEnded, XR_ERROR_RUNTIME_FAILURE);
				return;
			}
		}
		catch (...)
		{
			release();
			throw;
		}
		operation.display = *tick;
		operation.display->reprojection.source.reset();
		operation.capture = capture.id;
		operation.frame = tick->source;
		operation.phase = DebugPhase::Complete;
		operation.progress = DebugProgress::Displayed;
		m_session->Captures().Poll();
		return;
	}
	if (Admission(command))
	{
		if (OtherAdmission(operation.id))
		{
			Fail(operation, DebugProgress::Busy, XR_ERROR_CALL_ORDER_INVALID);
			return;
		}
		auto records = Candidates(operation);
		if (command == DebugCommand::Step && operation.request.additionalTargets.empty() &&
			operation.request.target == CaptureTarget{} && records.empty())
		{
			const auto previous = m_session->Captures().Snapshot().captures;
			std::optional<FrameKey> after;
			for (const auto& cursor : m_cursors)
				if (cursor.target == CaptureTarget{})
					after = cursor.frame;
			const auto boundary = m_session->CaptureBoundary(after);
			operation.retention = boundary.result;
			if (boundary.result != SnapshotAdmission::Accepted)
			{
				Fail(operation,
					 boundary.result == SnapshotAdmission::Unsupported ? DebugProgress::Unsupported
																	   : DebugProgress::RetentionFailure,
					 XR_ERROR_LIMIT_REACHED);
				return;
			}
			for (const auto capture : boundary.captures)
				if (std::find(previous.begin(), previous.end(), capture) == previous.end())
					Own(operation.lease, capture);
			m_session->Captures().Poll();
			records = Candidates(operation);
		}
		if (command == DebugCommand::Step && !records.empty() && records.front().frame &&
			Settled(records.front().phase))
		{
			operation.phase = DebugPhase::Pending;
			Inspect(operation, records.front());
			return;
		}
		if (command == DebugCommand::Step && !records.empty() && records.front().phase == SnapshotPhase::GpuPending)
		{
			operation.phase = DebugPhase::Pending;
			Settle(operation);
			return;
		}
		const auto needed = command == DebugCommand::StrictAdmission
								? 1U
								: static_cast<std::uint32_t>(operation.request.lookahead > records.size()
																 ? operation.request.lookahead - records.size()
																 : 0);
		operation.captures.reserve(needed * (operation.request.additionalTargets.size() + 1));
		for (std::uint32_t index = 0; index < needed; ++index)
		{
			auto targets = operation.request.additionalTargets;
			targets.insert(targets.begin(), operation.request.target);
			const auto capture = m_session->ReserveCaptureBundle(targets);
			operation.retention = capture.result;
			if (capture.result != SnapshotAdmission::Accepted)
			{
				m_session->CancelCaptureReservations();
				Fail(operation,
					 capture.result == SnapshotAdmission::Unsupported ? DebugProgress::Unsupported
																	  : DebugProgress::RetentionFailure,
					 XR_ERROR_LIMIT_REACHED);
				return;
			}
			for (const auto id : capture.captures)
			{
				Own(operation.lease, id);
				operation.captures.push_back(id);
			}
		}
		if (needed)
		{
			const auto granted = m_session->Scheduler().GrantPermits(needed);
			if (XR_FAILED(granted))
			{
				m_session->CancelCaptureReservations();
				Fail(operation, DebugProgress::RetentionFailure, granted);
				return;
			}
		}
		operation.phase = DebugPhase::Pending;
		operation.progress = DebugProgress::WaitingSubmission;
		Settle(operation);
	}
}
} // namespace simxr
