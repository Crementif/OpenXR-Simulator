#pragma once

#include "control/InspectionDocument.h"
#include "presentation/FrameHistory.h"

namespace simxr::control
{
InspectionValue InspectDiagnostics(const FrameDiagnostics& diagnostics);
InspectionValue InspectProjectionHistory(const FrameHistorySnapshot& history, std::size_t count);

// Copies historical CPU state only. Never queries a live registry, copies a GPU
// image, advances admission, or makes a native handle into a wire identity.
std::shared_ptr<const InspectionRecord> InspectFrame(InspectionStore& store, ControlSessionId session,
													 const FrameRecord& frame);
} // namespace simxr::control
