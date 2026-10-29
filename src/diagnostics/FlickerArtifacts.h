#pragma once

#include "diagnostics/FlickerMonitors.h"

#include <variant>

namespace flicker
{
struct PreviewIncident
{
	IncidentPacket packet;
	PreviewStatistics statistics;
};

struct UiIncident
{
	IncidentPacket packet;
	UiStatistics statistics;
};

using Artifact = std::variant<PreviewIncident, UiIncident, SampleArtifact>;

struct ArtifactCharge
{
	size_t bytes = 0;
	size_t samples = 0;
};

struct ArtifactJob
{
	Artifact artifact;
	ArtifactCharge charge;
};

// The Diagnostics mutex guards this queue. Dequeue keeps the active job charged
// until its owned pixels and metadata have been destroyed by the worker.
class ArtifactQueue
{
public:
	void Add(IncidentUpdate update, const PreviewMonitor& monitor);
	void Add(IncidentUpdate update, const UiMonitor& monitor);

	bool Empty() const;
	std::optional<ArtifactJob> Take();
	void Complete(ArtifactCharge charge);

	size_t Outstanding() const;
	size_t OutstandingBytes() const;
	uint64_t Dropped() const;

	bool TakeRenderDocRequest();

private:
	void Admit(Artifact artifact);

	std::deque<ArtifactJob> m_jobs;
	size_t m_outstanding = 0;
	size_t m_bytes = 0;
	size_t m_samples = 0;
	uint64_t m_dropped = 0;
	bool m_renderDocRequested = false;
};

void WriteArtifact(const Artifact& artifact);
void WritePreviewStatus(const std::filesystem::path& directory, const DiagnosticsStatus& status);
void WriteUiStatus(const std::filesystem::path& directory, const DiagnosticsStatus& status);
void PruneArtifacts(const std::filesystem::path& directory, std::string_view activeSession);
} // namespace flicker
