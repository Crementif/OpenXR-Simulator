#include "app/services/ManualInputController.h"
#include "Pch.h"
#include "app/services/WorkerConnection.h"
#include "control/ControlSchema.h"

namespace simxr::app
{
namespace wire = control;
using Clock = std::chrono::steady_clock;

static void Check(const wire::Message& reply)
{
	const auto error = reply.Find(wire::tag::Error);
	if (!error || error->AsU32() != 0)
	{
		const auto detail = reply.Find(wire::tag::Detail);
		throw std::runtime_error(detail && detail->AsText() ? std::string(*detail->AsText())
															: "Manual input request failed");
	}
}

static bool Pending(const wire::Message& result)
{
	const auto phase = SimulationEditPhase(result);
	return phase == "queued" || phase == "rescheduled";
}

ManualInputController::ManualInputController()
{
	m_worker = std::thread([this] { Run(); });
}

ManualInputController::~ManualInputController()
{
	{
		std::lock_guard lock(m_mutex);
		m_stopping = true;
		if (m_current)
			m_current->Stop();
	}
	m_changed.notify_all();
	m_worker.join();
}

void ManualInputController::Run() noexcept
{
	try
	{
		RunChecked();
	}
	catch (...)
	{
		std::shared_ptr<WorkerConnection> retired;
		{
			std::lock_guard lock(m_mutex);
			m_stopping = true;
			m_snapshot.error.swap(m_workerFailure);
			m_snapshot.posePending = false;
			m_snapshot.editPending = false;
			m_snapshot.source.reset();
			m_pose.reset();
			m_edits.clear();
			retired = std::move(m_current);
		}
	}
}

void ManualInputController::RunChecked()
{
	std::shared_ptr<WorkerConnection> connection;
	std::uint64_t bound = 0, transaction = 1;
	wire::Message observed, receipt;
	std::optional<wire::Message> waiting;
	bool waitingPose = false;
	auto nextRead = Clock::time_point::max();
	while (true)
	{
		std::shared_ptr<WorkerConnection> retired;
		ManualInputTarget selected;
		std::optional<SimulationEdit> edit;
		bool read = false;
		{
			std::unique_lock lock(m_mutex);
			m_changed.wait_until(lock, nextRead, [&] {
				return m_stopping || bound != m_revision || m_refresh ||
					   (!m_edits.empty() &&
						(!waiting || std::holds_alternative<ReleaseManualInputs>(m_edits.front()))) ||
					   (m_pose && !waiting);
			});
			if (m_stopping)
				return;
			if (bound != m_revision)
			{
				bound = m_revision;
				retired = std::exchange(connection, nullptr);
				m_current.reset();
				waiting.reset();
				observed = receipt = {};
				transaction = 1;
				nextRead = Clock::now();
			}
			selected = m_target;
			if (selected.endpoint.empty() || !selected.session)
			{
				m_refresh = false;
				nextRead = Clock::time_point::max();
				continue;
			}
			if (!m_edits.empty() && (!waiting || std::holds_alternative<ReleaseManualInputs>(m_edits.front())))
			{
				edit = std::move(m_edits.front());
				m_edits.pop_front();
			}
			else if (m_refresh || Clock::now() >= nextRead)
				read = true;
			else if (m_pose && !waiting)
			{
				edit = std::move(m_pose);
				m_pose.reset();
			}
			m_refresh = false;
		}
		retired.reset();
		try
		{
			if (!connection)
			{
				connection = std::make_shared<WorkerConnection>(
					selected.endpoint, "simxr_manual_input",
					static_cast<std::uint64_t>(wire::Capability::SimulationCommands));
				std::lock_guard lock(m_mutex);
				if (m_stopping || bound != m_revision)
					connection->Stop();
				m_current = connection;
			}
			if (edit)
			{
				auto packet = EncodeSimulationEdit(*edit, selected.session, selected.generation, transaction, observed);
				auto result = connection->Exchange(static_cast<wire::OpCode>(packet.message.opcode),
												   std::move(packet.message.fields));
				Check(result);
				if (std::holds_alternative<ReleaseManualInputs>(*edit))
				{
					waiting.reset();
					observed = std::move(result);
					receipt = {};
				}
				else
				{
					++transaction;
					receipt = std::move(result);
					{
						if (!Pending(receipt) && SimulationEditPhase(receipt) != "applied")
							throw std::runtime_error("Input rejected: " + SimulationEditPhase(receipt));
						waiting = receipt;
						const auto& commands = std::get<ManualTransaction>(*edit).commands;
						waitingPose =
							std::any_of(commands.begin(), commands.end(), [](const SimulationCommand& command) {
								return std::holds_alternative<SetHeadPose>(command) ||
									   std::holds_alternative<SetControllerPose>(command) ||
									   std::holds_alternative<SetTracker>(command) ||
									   std::holds_alternative<SetTrackers>(command);
							});
					}
				}
				nextRead = Clock::now();
			}
			if (read)
			{
				observed = connection->Exchange(
					wire::OpCode::SimulationStatus,
					{wire::Field::U64(wire::tag::Session, selected.session, true),
					 wire::Field::U64(wire::tag::Generation, selected.generation, true),
					 wire::Field::U32(wire::tag::SourceKind, static_cast<std::uint32_t>(CommandSourceKind::Manual))});
				Check(observed);
				if (const auto applied = ObservedEditResult(observed, receipt))
					receipt = *applied;
				if (waiting)
					if (const auto outcome = ObservedEditResult(observed, *waiting); outcome && !Pending(*outcome))
					{
						waiting.reset();
						if (SimulationEditPhase(*outcome) != "applied")
							throw std::runtime_error("Input rejected: " + SimulationEditPhase(*outcome) +
													 ". Check which script or replay controls the device.");
						std::lock_guard lock(m_mutex);
						if (bound == m_revision && waitingPose)
							++m_snapshot.appliedPoses;
					}
				nextRead = Clock::now() + std::chrono::milliseconds(waiting ? 4 : 500);
			}
			std::lock_guard lock(m_mutex);
			if (bound == m_revision)
			{
				m_snapshot.posePending = waiting.has_value() || m_pose.has_value();
				m_snapshot.editPending = m_snapshot.posePending || !m_edits.empty();
				m_snapshot.state = observed;
				m_snapshot.result = receipt;
				if (auto source = SimulationEditSource(receipt))
					m_snapshot.source = source;
				if (read)
					++m_snapshot.observation;
			}
		}
		catch (const std::exception& error)
		{
			waiting.reset();
			{
				std::lock_guard lock(m_mutex);
				if (bound == m_revision)
				{
					m_snapshot.error = error.what();
					m_snapshot.posePending = false;
					m_snapshot.editPending = false;
					m_pose.reset();
					m_edits.clear();
					m_snapshot.source.reset();
				}
				m_current.reset();
			}
			// Disconnect releases held inputs; retire the transport outside the UI gate.
			connection.reset();
			nextRead = Clock::now() + std::chrono::milliseconds(500);
		}
	}
}

void ManualInputController::SetTarget(ManualInputTarget target)
{
	std::lock_guard lock(m_mutex);
	if (m_stopping || m_target == target)
		return;
	m_target = std::move(target);
	++m_revision;
	m_snapshot = {};
	m_edits.clear();
	m_pose.reset();
	if (m_current)
		m_current->Stop();
	m_changed.notify_all();
}

bool ManualInputController::Submit(SimulationEdit edit, bool continuous)
{
	std::lock_guard lock(m_mutex);
	if (m_stopping || m_target.endpoint.empty() || !m_target.session)
		return false;
	if (const auto transaction = std::get_if<ManualTransaction>(&edit))
	{
		continuous |= transaction->continuous;
		if (!continuous && m_edits.size() + (m_pose ? 1 : 0) >= 32)
		{
			m_snapshot.error = "Too many inputs. Release the controls and try again.";
			if (!transaction->releaseEdge)
				return false;
			// A safety release retires queued presses rather than losing a release edge.
			edit = ReleaseManualInputs{};
		}
	}
	if (std::holds_alternative<ReleaseManualInputs>(edit))
	{
		m_edits.clear();
		m_pose.reset();
	}
	m_snapshot.error.clear();
	m_snapshot.editPending = true;
	if (continuous)
	{
		m_pose = std::move(edit);
		m_snapshot.posePending = true;
	}
	else
	{
		if (m_pose)
		{
			m_edits.push_back(std::move(*m_pose));
			m_pose.reset();
		}
		m_edits.push_back(std::move(edit));
	}
	m_changed.notify_all();
	return true;
}

void ManualInputController::DiscardUnsentPose()
{
	std::lock_guard lock(m_mutex);
	m_pose.reset();
}

void ManualInputController::Refresh()
{
	std::lock_guard lock(m_mutex);
	if (m_stopping)
		return;
	m_refresh = true;
	m_snapshot.error.clear();
	m_changed.notify_all();
}

ManualInputSnapshot ManualInputController::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return m_snapshot;
}
} // namespace simxr::app
