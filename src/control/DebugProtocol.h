#pragma once

#include "control/WireProtocol.h"
#include "presentation/DebugController.h"

#include <optional>
#include <string>
#include <vector>

namespace simxr
{
struct SessionSnapshot;
}

namespace simxr::control
{
void EncodeFrameHistory(Message& response, const FrameHistorySnapshot& history, std::uint64_t after, std::size_t count,
						bool trackerCollections = false);
std::optional<std::vector<std::string>> DecodeCompatibilityDeviations(const Message& status);
const char* SnapshotPhaseName(SnapshotPhase phase);
Field EncodeCaptureTarget(CaptureTarget target);
CaptureTarget DecodeCaptureTarget(const Message& request);
Field EncodeFrameKey(std::uint16_t tag, FrameKey frame);
Field EncodeDisplayTick(std::uint16_t tag, const PresentationTick& tick);
DebugRequest DecodeDebugRequest(const Message& request);
void EncodeDebugAdmission(Message& response, const DebugSubmission& submission);
void EncodeDebuggerStatus(Message& response, const DebugControllerSnapshot& debugger, const SessionSnapshot& session,
						  const PresentationSnapshot& display, std::uint64_t capabilities);
void EncodeDebugOperation(Message& response, const DebugOperation& operation);
void EncodeCaptureMetadata(Message& response, const SnapshotRecord& capture, bool trackerCollections = false);
} // namespace simxr::control
