#pragma once

#include "control/InspectionDocument.h"
#include "presentation/FrameHistory.h"
#include "presentation/FrameImageHistory.h"

#include <cstddef>
#include <memory>

namespace simxr::control
{
InspectionValue InspectPreviewTracking(const TrackingSnapshot& tracking);
InspectionValue InspectDiagnostics(const FrameDiagnostics& diagnostics);
InspectionValue InspectImageWindow(const ImageWindowSnapshot& window);
InspectionValue InspectImageHistory(const ImageHistorySnapshot& history);
InspectionValue InspectProjectionHistory(const FrameHistorySnapshot& history, std::size_t count);

// Copies historical CPU state only. Never queries a live registry, copies a GPU
// image, advances admission, or makes a native handle into a wire identity.
std::shared_ptr<const InspectionRecord> InspectFrame(InspectionStore& store, ControlSessionId session,
													 const FrameRecord& frame);
} // namespace simxr::control
