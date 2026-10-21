#pragma once

#include "control/ProtocolTypes.h"

#include <cstdint>
#include <span>

namespace simxr::control
{
struct ControlPacket;
struct Message;
enum class RequestDomain
{
	Connection,
	Debugger,
	Inspection,
	Images,
	Simulation,
	Game
};
struct RequestSchema
{
	OpCode opcode;
	RequestDomain domain;
	std::uint16_t minimumMinor;
	std::span<const std::uint16_t> fields;
};
const RequestSchema& ValidateControlRequest(const ControlPacket& packet, bool nativeHandles);
std::uint64_t RequireRequestU64(const Message& request, std::uint16_t tag);
std::uint32_t RequireRequestU32(const Message& request, std::uint16_t tag);
namespace tag
{
inline constexpr std::uint16_t Error = 1, Detail = 2, Session = 3, Generation = 4, Lease = 5, CommandId = 6,
							   OperationId = 7, CaptureId = 8, ClientName = 10, RequiredCapabilities = 11,
							   OptionalCapabilities = 12, Minor = 13, Capabilities = 14, PayloadLimit = 15,
							   Sessions = 20, State = 21, Running = 22, Closed = 23, Manual = 24, Lookahead = 25,
							   LedgerCount = 26, Pipeline = 30, Retention = 31, Presentation = 32, CommandKind = 41,
							   Target = 42, CaptureKind = 50, Layer = 51, View = 52, Phase = 60, Progress = 61,
							   Result = 62, RetentionResult = 63, Frame = 64, Captures = 65, Diagnostics = 66,
							   Outstanding = 67, Display = 68, Prediction = 70, FrameId = 71, Tick = 72, Time = 73,
							   Period = 74, Stage = 75, Submission = 76, OutputDisposition = 80, DropReason = 81,
							   LayerCount = 82, LeaseRemaining = 83, Duplicate = 84, WaitPending = 85, Paused = 86,
							   Permits = 87, Waited = 88, Begun = 89, NextAdmission = 90, Capacity = 91, HeldSlots = 92,
							   Retiring = 93, LastSubmitted = 94, CoalescedClears = 95, Kind = 96, SkippedTicks = 97,
							   ImageAvailable = 98, Source = 99, RevisionId = 100, InspectionId = 101, Document = 102,
							   RevisionKind = 103, Released = 104, ImageReaderId = 105, DeliveryId = 106,
							   PoolGeneration = 107, SharedImageId = 108, ExportId = 109, DeviceUuid = 110,
							   DriverUuid = 111, ImageDescriptor = 112, Format = 115, Width = 116, Height = 117,
							   Depth = 118, ArrayLayers = 119, MipLevels = 120, Samples = 121, Usage = 122,
							   Tiling = 123, ImageType = 124, ImageFlags = 125, AllocationSize = 126,
							   MemoryTypeIndex = 127, Dedicated = 128, ProducerQueueFamily = 129, Layout = 130,
							   ProducerPhase = 131, ConsumerPhase = 132, Offset = 133, Length = 134, Bytes = 135,
							   TotalBytes = 136, End = 137, HandleKind = 138, HandleCount = 139, Commands = 140,
							   TransactionId = 141, ApplyFrame = 142, ApplyTime = 143, LatePolicy = 144,
							   SourceKind = 145, TakeControl = 146, Channel = 147, Values = 148, Enabled = 149,
							   Pose = 150, Position = 151, Orientation = 152, Controls = 153, Proportions = 154,
							   Scenario = 155, Seed = 156, Overflow = 158, Recording = 159, Ownership = 160,
							   Transactions = 161, Revision = 162, AppliedFrame = 163, AppliedTime = 164,
							   Rescheduled = 165, GameId = 166, GameKind = 167;
}

enum class Capability : std::uint64_t
{
	DebugCommands = 1ULL << 0,
	SnapshotMetadata = 1ULL << 1,
	PipelineInspection = 1ULL << 2,
	ComposedCapture = 1ULL << 3,
	LayerViewCapture = 1ULL << 4,
	GpuPreview = 1ULL << 5,
	ImageExport = 1ULL << 6,
	FullFrameState = 1ULL << 7,
	HistoricalCpuState = 1ULL << 8,
	ImmutableRevisions = 1ULL << 9,
	LivePreview = 1ULL << 10,
	SimulationCommands = 1ULL << 11,
	CommandRecording = 1ULL << 12,
	FrameDiagnostics = 1ULL << 13,
	GameBridge = 1ULL << 14
};

// Wire values are explicit and independent of the core's enum representation.
enum class CommandKind : std::uint32_t
{
	Pause = 1,
	Step = 2,
	StrictAdmission = 3,
	DisplayStep = 4,
	Resume = 5,
	Cancel = 6,
	Release = 7
};
enum class TargetKind : std::uint32_t
{
	ComposedEyes = 1,
	LayerView = 2,
	VirtualDisplay = 3
};
} // namespace simxr::control
