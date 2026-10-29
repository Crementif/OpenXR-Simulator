#pragma once

#include "automation/Recording.h"

#include <filesystem>
#include <stop_token>

namespace simxr::automation
{
struct RecordingEvidenceEntry
{
	GameObservation observation;
	std::optional<FrameId> first, last;
};

struct RecordingEvidence
{
	bool available = false, complete = false, gap = false, correlated = false, more = false;
	std::uint64_t first = 0, next = 0;
	GameBridgeSnapshot initial;
	std::vector<RecordingEvidenceEntry> entries;
};

// Bounded journal page plus conservative surrounding recorded input boundaries.
RecordingEvidence ReadRecordingEvidence(const std::filesystem::path& directory, std::uint64_t cursor = 0,
										std::stop_token stop = {});
} // namespace simxr::automation
