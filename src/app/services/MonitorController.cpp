#include "Pch.h"
#include "app/services/MonitorController.h"
#include "app/services/ApplicationCatalog.h"
#include "automation/MonitorSource.h"
#include "control/ControlSchema.h"

#include <iomanip>

namespace simxr::app
{
MonitorController::MonitorController()
	: m_runner([this](auto opcode, auto fields) { return m_transport->Exchange(opcode, std::move(fields)); },
			   [this] { m_transport->Close(); })
{}

MonitorController::~MonitorController()
{
	Stop();
}

void MonitorController::Start(const ApplicationSnapshot& application, MonitorOptions options)
{
	if (options.id.empty() || options.id.size() > 128 || options.id.find('\0') != std::string::npos ||
		(options.target != "composed" && options.target != "ui") || !std::isfinite(options.interval) ||
		options.interval < .25 || options.interval > 30 || !std::isfinite(options.timeout) || options.timeout <= 0 ||
		options.timeout > 30)
		throw std::invalid_argument("Invalid monitor identifier, target, interval or timeout");
	if (m_status.options.id == options.id && m_status.session)
	{
		if (m_status.options != options)
			throw std::invalid_argument("monitor_id was already used with different options");
		return;
	}
	if (Active())
		throw std::runtime_error("A monitor is already running for this application");
	if (!application.connected || !application.running || !application.runtimeProcess || !application.session)
		throw std::runtime_error("Monitoring requires a connected running session");
	Stop();
	m_transport = std::make_unique<WorkerConnection>(application.endpoint, "Automatic monitoring",
													 static_cast<std::uint64_t>(control::Capability::ImageRegionExport),
													 application.runtimeProcess);
	automation::RigOptions run;
	run.session = {application.session};
	run.generation = application.generation;
	run.name = "automatic-monitor.luau";
	run.background = true;
	run.deadline = std::chrono::milliseconds{static_cast<long long>(std::ceil(options.timeout * 1000))};
	run.memoryLimit = 4 * 1024 * 1024;
	std::ostringstream source;
	source.imbue(std::locale::classic());
	source << std::setprecision(17) << "local target = \"" << (options.target == "ui" ? "quad_layers" : "composed")
		   << "\"\nlocal interval = " << options.interval << "\nlocal timeout = " << options.timeout << '\n'
		   << automation::MonitorSource;
	run.source = source.str();
	m_status = {std::move(options),	   {}, application.runtimeProcess, application.runtimeEpoch, application.session,
				application.generation};
	m_runner.Start(std::move(run));
}

void MonitorController::Stop()
{
	m_runner.Cancel();
	m_runner.Join();
	m_transport.reset();
}

bool MonitorController::Active() const
{
	const auto phase = m_runner.Phase();
	return phase == automation::RigPhase::Running || phase == automation::RigPhase::Paused;
}

bool MonitorController::Matches(const ApplicationSnapshot& application) const
{
	return application.running && application.connected && m_status.process == application.runtimeProcess &&
		   m_status.epoch == application.runtimeEpoch && m_status.session == application.session &&
		   m_status.generation == application.generation;
}

MonitorStatus MonitorController::Status() const
{
	auto result = m_status;
	result.run = m_runner.Snapshot();
	return result;
}
}
