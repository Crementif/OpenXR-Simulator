#include "app/ManualInputController.h"
#include "app/WorkerConnection.h"
#include "control/ControlSchema.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace simxr::app
{
namespace
{
namespace wire = control;
using Clock = std::chrono::steady_clock;
void Check(const wire::Message& reply)
{
	const auto error = reply.Find(wire::tag::Error);
	if (!error || error->AsU32() != 0)
	{
		const auto detail = reply.Find(wire::tag::Detail);
		throw std::runtime_error(detail && detail->AsText() ? std::string(*detail->AsText())
															: "Manual input request failed");
	}
}
bool Pending(const wire::Message& result)
{
	const auto phase = SimulationEditPhase(result);
	return phase == "queued" || phase == "rescheduled";
}
} // namespace
class ManualInputController::State
{
  public:
	mutable std::mutex mutex;
	std::condition_variable changed;
	ManualInputTarget target;
	ManualInputSnapshot snapshot;
	std::deque<SimulationEdit> edits;
	std::optional<SimulationEdit> pose;
	std::shared_ptr<WorkerConnection> current;
	std::uint64_t revision = 0;
	bool stopping = false, refresh = false;
	std::thread worker;

	State()
		: worker(
			  [this]
			  {
				  Run();
			  })
	{
	}
	~State()
	{
		{
			std::lock_guard lock(mutex);
			stopping = true;
			if (current)
				current->Stop();
		}
		changed.notify_all();
		worker.join();
	}
	void Run()
	{
		std::shared_ptr<WorkerConnection> connection;
		std::uint64_t bound = 0, transaction = 1;
		wire::Message observed, receipt;
		std::optional<wire::Message> waiting;
		bool waitingPose = false;
		auto nextRead = Clock::time_point::max();
		for (;;)
		{
			std::shared_ptr<WorkerConnection> retired;
			ManualInputTarget selected;
			std::optional<SimulationEdit> edit;
			bool read = false;
			{
				std::unique_lock lock(mutex);
				changed.wait_until(
					lock, nextRead,
					[&]
					{
						return stopping || bound != revision || refresh ||
							   (!edits.empty() &&
								(!waiting || std::holds_alternative<ReleaseManualInputs>(edits.front()))) ||
							   (pose && !waiting);
					});
				if (stopping)
					return;
				if (bound != revision)
				{
					bound = revision;
					retired = std::move(connection);
					current.reset();
					waiting.reset();
					observed = receipt = {};
					transaction = 1;
					nextRead = Clock::now();
				}
				selected = target;
				if (selected.endpoint.empty() || !selected.session)
				{
					refresh = false;
					nextRead = Clock::time_point::max();
					continue;
				}
				if (!edits.empty() && (!waiting || std::holds_alternative<ReleaseManualInputs>(edits.front())))
				{
					edit = std::move(edits.front());
					edits.pop_front();
				}
				else if (refresh || Clock::now() >= nextRead)
					read = true;
				else if (pose && !waiting)
				{
					edit = std::move(pose);
					pose.reset();
				}
				refresh = false;
			}
			retired.reset();
			try
			{
				if (!connection)
				{
					connection = std::make_shared<WorkerConnection>(
						selected.endpoint, "simxr_manual_input",
						static_cast<std::uint64_t>(wire::Capability::SimulationCommands));
					std::lock_guard lock(mutex);
					if (stopping || bound != revision)
						connection->Stop();
					current = connection;
				}
				if (edit)
				{
					auto packet =
						EncodeSimulationEdit(*edit, selected.session, selected.generation, transaction, observed);
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
							waitingPose = std::any_of(commands.begin(), commands.end(),
													  [](const SimulationCommand& command)
													  {
														  return std::holds_alternative<SetHeadPose>(command) ||
																 std::holds_alternative<SetControllerPose>(command) ||
																 std::holds_alternative<SetTracker>(command);
													  });
						}
					}
					nextRead = Clock::now();
				}
				if (read)
				{
					observed =
						connection->Exchange(wire::OpCode::SimulationStatus,
											 {wire::Field::U64(wire::tag::Session, selected.session, true),
											  wire::Field::U64(wire::tag::Generation, selected.generation, true),
											  wire::Field::U32(wire::tag::SourceKind,
															   static_cast<std::uint32_t>(CommandSourceKind::Manual))});
					Check(observed);
					if (const auto applied = ObservedEditResult(observed, receipt))
						receipt = *applied;
					if (waiting)
						if (const auto outcome = ObservedEditResult(observed, *waiting); outcome && !Pending(*outcome))
						{
							waiting.reset();
							if (SimulationEditPhase(*outcome) != "applied")
								throw std::runtime_error("Input rejected: " + SimulationEditPhase(*outcome) +
														 ". Check input ownership.");
							std::lock_guard lock(mutex);
							if (bound == revision && waitingPose)
								++snapshot.appliedPoses;
						}
					nextRead = Clock::now() + std::chrono::milliseconds(waiting ? 4 : 500);
				}
				std::lock_guard lock(mutex);
				if (bound == revision)
				{
					snapshot.posePending = waiting.has_value() || pose.has_value();
					snapshot.editPending = snapshot.posePending || !edits.empty();
					snapshot.state = observed;
					snapshot.result = receipt;
					if (auto source = SimulationEditSource(receipt))
						snapshot.source = source;
					if (read)
						++snapshot.observation;
				}
			}
			catch (const std::exception& error)
			{
				waiting.reset();
				{
					std::lock_guard lock(mutex);
					if (bound == revision)
					{
						snapshot.error = error.what();
						snapshot.posePending = false;
						snapshot.editPending = false;
						pose.reset();
						edits.clear();
						snapshot.source.reset();
					}
					current.reset();
				}
				// Disconnect releases held inputs; retire the transport outside the UI gate.
				connection.reset();
				nextRead = Clock::now() + std::chrono::milliseconds(500);
			}
		}
	}
};
ManualInputController::ManualInputController() : m_state(std::make_unique<State>()) {}
ManualInputController::~ManualInputController() = default;
void ManualInputController::SetTarget(ManualInputTarget target)
{
	std::lock_guard lock(m_state->mutex);
	if (m_state->target == target)
		return;
	m_state->target = std::move(target);
	++m_state->revision;
	m_state->snapshot = {};
	m_state->edits.clear();
	m_state->pose.reset();
	if (m_state->current)
		m_state->current->Stop();
	m_state->changed.notify_all();
}
bool ManualInputController::Submit(SimulationEdit edit, bool continuous)
{
	std::lock_guard lock(m_state->mutex);
	if (m_state->target.endpoint.empty() || !m_state->target.session)
		return false;
	if (const auto transaction = std::get_if<ManualTransaction>(&edit))
	{
		continuous |= transaction->continuous;
		if (!continuous && m_state->edits.size() + (m_state->pose ? 1 : 0) >= 32)
		{
			m_state->snapshot.error = "Input queue full — release inputs";
			if (!transaction->releaseEdge)
				return false;
			// A safety release retires queued presses rather than losing a release edge.
			edit = ReleaseManualInputs{};
		}
	}
	if (std::holds_alternative<ReleaseManualInputs>(edit))
	{
		m_state->edits.clear();
		m_state->pose.reset();
	}
	m_state->snapshot.error.clear();
	m_state->snapshot.editPending = true;
	if (continuous)
	{
		m_state->pose = std::move(edit);
		m_state->snapshot.posePending = true;
	}
	else
	{
		if (m_state->pose)
		{
			m_state->edits.push_back(std::move(*m_state->pose));
			m_state->pose.reset();
		}
		m_state->edits.push_back(std::move(edit));
	}
	m_state->changed.notify_all();
	return true;
}
void ManualInputController::DiscardUnsentPose()
{
	std::lock_guard lock(m_state->mutex);
	m_state->pose.reset();
}
void ManualInputController::Refresh()
{
	std::lock_guard lock(m_state->mutex);
	m_state->refresh = true;
	m_state->snapshot.error.clear();
	m_state->changed.notify_all();
}
ManualInputSnapshot ManualInputController::Snapshot() const
{
	std::lock_guard lock(m_state->mutex);
	return m_state->snapshot;
}
} // namespace simxr::app
