#pragma once

#include "app/services/WorkerConnection.h"
#include "automation/RigRunner.h"

#include <memory>
#include <string>

namespace simxr::app
{
struct ApplicationSnapshot;

struct MonitorOptions
{
	std::string id, target = "composed";
	double interval = .25, timeout = 10;
	bool operator==(const MonitorOptions&) const = default;
};

struct MonitorStatus
{
	MonitorOptions options;
	automation::RigSnapshot run;
	std::uint64_t process = 0, epoch = 0, session = 0, generation = 0;
};

class MonitorController
{
public:
	MonitorController();
	~MonitorController();

	void Start(const ApplicationSnapshot& application, MonitorOptions options);
	void Stop();

	bool Active() const;
	bool Matches(const ApplicationSnapshot& application) const;
	MonitorStatus Status() const;

private:
	std::unique_ptr<WorkerConnection> m_transport;
	automation::RigRunner m_runner;
	MonitorStatus m_status;
};
}
