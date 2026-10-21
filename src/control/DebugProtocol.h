#pragma once

#include "control/WireProtocol.h"
#include "presentation/DebugController.h"

namespace simxr
{
struct SessionSnapshot;
}
namespace simxr::control
{
Field EncodeCaptureTarget(CaptureTarget target);
CaptureTarget DecodeCaptureTarget(const Message& request);
Field EncodeFrameKey(std::uint16_t tag, FrameKey frame);
Field EncodeDisplayTick(std::uint16_t tag, const PresentationTick& tick);
DebugRequest DecodeDebugRequest(const Message& request);
void EncodeDebugAdmission(Message& response, const DebugSubmission& submission);
void EncodeDebuggerStatus(Message& response, const DebugControllerSnapshot& debugger, const SessionSnapshot& session,
						  const PresentationSnapshot& display, std::uint64_t capabilities);
void EncodeDebugOperation(Message& response, const DebugOperation& operation);
void EncodeCaptureMetadata(Message& response, const SnapshotRecord& capture);
} // namespace simxr::control
