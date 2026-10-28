#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

namespace simxr::control
{
inline constexpr std::uint32_t Magic = 0x32525853;
inline constexpr std::uint16_t ProtocolMajor = 1;
inline constexpr std::uint16_t ProtocolMinor = 17;
inline constexpr std::size_t HeaderSize = 32;
inline constexpr std::size_t MaxPayload = 1024 * 1024;
inline constexpr std::size_t MaxFields = 256;

struct RequestId
{
	std::uint64_t value = 0;
	auto operator<=>(const RequestId&) const = default;
};
struct ControlSessionId
{
	std::uint64_t value = 0;
	auto operator<=>(const ControlSessionId&) const = default;
};
struct ConnectionId
{
	std::uint64_t value = 0;
	auto operator<=>(const ConnectionId&) const = default;
};

enum class OpCode : std::uint16_t
{
	Hello = 1,
	Sessions = 2,
	Status = 3,
	Acquire = 4,
	Renew = 5,
	Relinquish = 6,
	Submit = 7,
	Operation = 8,
	Acknowledge = 9,
	Capture = 10,
	InspectFrame = 11,
	FeatureRevision = 12,
	ReleaseInspection = 13,
	RetainImage = 14,
	ImageInfo = 15,
	ShareImage = 16,
	DeliveryInfo = 17,
	ReleaseDelivery = 18,
	ReleaseImage = 19,
	StartExport = 20,
	ExportInfo = 21,
	ReadExport = 22,
	ReleaseExport = 23,
	RetainPreview = 24,
	SubmitSimulation = 25,
	SimulationStatus = 26,
	CancelSimulation = 27,
	ReleaseControl = 28,
	StartRecording = 29,
	StopRecording = 30,
	ReadRecording = 31,
	ReadDiagnostics = 32,
	GameStatus = 33,
	SubmitGame = 34,
	GameOperation = 35,
	ReadProjectionHistory = 36,
	ReadImageHistory = 37,
	RetainHistoricalImage = 38,
	BeginImageWindow = 39,
	ReadImageWindow = 40,
	RetainWindowImage = 41,
	ReleaseImageWindow = 42,
	UploadSequence = 43,
	StartSequence = 44,
	ReadSequence = 45,
	StopSequence = 46,
	ReleaseSequenceCapture = 47,
	ReadFrameHistory = 48,
	ReadHaptics = 49,
	ConfigurePreviews = 50,
	StartVideoExport = 51,
	AttachLiveInput = 52,
	SubmitLiveInput = 53,
	DetachLiveInput = 54,
	ReadLiveClock = 55,
	PublishDisplayPrediction = 56,
	AcknowledgeRecording = 57,
	ReadGameObservations = 58
};

enum class ErrorCode : std::uint32_t
{
	Ok = 0,
	Invalid = 1,
	UnsupportedVersion = 2,
	UnsupportedCommand = 3,
	NotNegotiated = 4,
	Capacity = 5,
	NotFound = 6,
	StaleGeneration = 7,
	LeaseRequired = 8,
	Conflict = 9,
	Busy = 10,
	Closed = 11,
	Internal = 12,
	UnsupportedField = 13
};
} // namespace simxr::control
