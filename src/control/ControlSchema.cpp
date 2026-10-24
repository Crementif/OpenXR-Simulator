#include "control/ControlSchema.h"
#include "Pch.h"
#include "control/ControlPacket.h"
#include "control/ControlRequestError.h"

namespace simxr::control
{
static constexpr std::array s_uploadSequence{tag::Session, tag::Generation, tag::SourceKind, tag::ProgramId,
											 tag::Offset,  tag::TotalBytes, tag::Bytes};
static constexpr std::array s_startSequence{tag::Session, tag::Generation, tag::SourceKind, tag::ProgramId, tag::Loop};
static constexpr std::array s_sequence{tag::Session,	tag::Generation, tag::SourceKind,
									   tag::SequenceId, tag::Offset,	 tag::Length};
static constexpr std::array s_sequenceCapture{tag::Session, tag::Generation, tag::CaptureId};
static constexpr std::array s_simulation{tag::Session,	 tag::Generation, tag::TransactionId, tag::ApplyFrame,
										 tag::ApplyTime, tag::LatePolicy, tag::SourceKind,	  tag::Commands};
static constexpr std::array s_simulationStatus{tag::Session, tag::Generation, tag::TransactionId, tag::SourceKind};
static constexpr std::array s_game{tag::Session,   tag::Generation,	 tag::GameId, tag::GameKind,
								   tag::CommandId, tag::OperationId, tag::Detail};
static constexpr std::array s_diagnostics{tag::Session, tag::Generation, tag::Offset, tag::Length, tag::FrameId};
static constexpr std::array s_historicalImage{tag::Session, tag::Generation, tag::FrameId, tag::Target};
static constexpr std::array s_imageHistory{tag::Session, tag::Generation, tag::Length, tag::Target};
static constexpr std::array s_beginImageWindow{tag::Session, tag::Generation, tag::Target};
static constexpr std::array s_frameHistory{tag::Session, tag::Generation, tag::Offset, tag::Length};
static constexpr std::array s_projectionHistory{tag::Session, tag::Generation, tag::Length};
static constexpr std::array s_recording{tag::Session, tag::Generation, tag::SourceKind, tag::Offset, tag::Length};
static constexpr std::array s_preview{tag::Session, tag::Generation, tag::FrameId};
static constexpr std::array s_common{tag::Session, tag::Generation};
static constexpr std::array s_hello{tag::ClientName, tag::RequiredCapabilities, tag::OptionalCapabilities};
static constexpr std::array s_submit{tag::Session,			 tag::Generation,		tag::Lease,	 tag::CommandId,
									 tag::CommandKind,		 tag::Lookahead,		tag::Target, tag::OperationId,
									 tag::CaptureId,		 tag::Capacity,			tag::Length, tag::AdvanceDisplay,
									 tag::SnapshotBudgetMiB, tag::AdditionalTargets};
static constexpr std::array s_leased{tag::Session, tag::Generation, tag::Lease};
static constexpr std::array s_operation{tag::Session, tag::Generation, tag::OperationId};
static constexpr std::array s_acknowledge{tag::Session, tag::Generation, tag::Lease, tag::CommandId};
static constexpr std::array s_capture{tag::Session, tag::Generation, tag::CaptureId};
static constexpr std::array s_retainImage{tag::Session, tag::Generation, tag::CaptureId, tag::Target};
static constexpr std::array s_revision{tag::RevisionId};
static constexpr std::array s_releaseInspection{tag::InspectionId};
static constexpr std::array s_imageWindow{tag::ImageWindowId};
static constexpr std::array s_windowImage{tag::ImageWindowId, tag::FrameId};
static constexpr std::array s_image{tag::ImageReaderId};
static constexpr std::array s_share{tag::ImageReaderId, tag::DeviceUuid, tag::DriverUuid, tag::CompletionKind};
static constexpr std::array s_delivery{tag::DeliveryId};
static constexpr std::array s_startExport{tag::ImageReaderId, tag::Layer};
static constexpr std::array s_exportId{tag::ExportId};
static constexpr std::array s_readExport{tag::ExportId, tag::Offset, tag::Length};
static constexpr std::array s_schemas{
	RequestSchema{OpCode::Hello, RequestDomain::Connection, 0, s_hello},
	RequestSchema{OpCode::Sessions, RequestDomain::Connection, 0, {}},
	RequestSchema{OpCode::Status, RequestDomain::Debugger, 0, s_common},
	RequestSchema{OpCode::Acquire, RequestDomain::Debugger, 0, s_common},
	RequestSchema{OpCode::Renew, RequestDomain::Debugger, 0, s_leased},
	RequestSchema{OpCode::Relinquish, RequestDomain::Debugger, 0, s_leased},
	RequestSchema{OpCode::Submit, RequestDomain::Debugger, 0, s_submit},
	RequestSchema{OpCode::Operation, RequestDomain::Debugger, 0, s_operation},
	RequestSchema{OpCode::Acknowledge, RequestDomain::Debugger, 0, s_acknowledge},
	RequestSchema{OpCode::Capture, RequestDomain::Inspection, 0, s_capture},
	RequestSchema{OpCode::InspectFrame, RequestDomain::Inspection, 0, s_capture},
	RequestSchema{OpCode::ReadFrameHistory, RequestDomain::Inspection, 4, s_frameHistory},
	RequestSchema{OpCode::ReadProjectionHistory, RequestDomain::Inspection, 4, s_projectionHistory},
	RequestSchema{OpCode::BeginImageWindow, RequestDomain::Inspection, 4, s_beginImageWindow},
	RequestSchema{OpCode::ReadImageWindow, RequestDomain::Images, 4, s_imageWindow},
	RequestSchema{OpCode::ReleaseImageWindow, RequestDomain::Images, 4, s_imageWindow},
	RequestSchema{OpCode::RetainWindowImage, RequestDomain::Images, 4, s_windowImage},
	RequestSchema{OpCode::ReadImageHistory, RequestDomain::Inspection, 4, s_imageHistory},
	RequestSchema{OpCode::RetainHistoricalImage, RequestDomain::Inspection, 4, s_historicalImage},
	RequestSchema{OpCode::FeatureRevision, RequestDomain::Inspection, 0, s_revision},
	RequestSchema{OpCode::ReleaseInspection, RequestDomain::Inspection, 0, s_releaseInspection},
	RequestSchema{OpCode::RetainImage, RequestDomain::Images, 2, s_retainImage},
	RequestSchema{OpCode::ImageInfo, RequestDomain::Images, 2, s_image},
	RequestSchema{OpCode::ReleaseImage, RequestDomain::Images, 2, s_image},
	RequestSchema{OpCode::ShareImage, RequestDomain::Images, 2, s_share},
	RequestSchema{OpCode::DeliveryInfo, RequestDomain::Images, 2, s_delivery},
	RequestSchema{OpCode::ReleaseDelivery, RequestDomain::Images, 2, s_delivery},
	RequestSchema{OpCode::StartExport, RequestDomain::Images, 2, s_startExport},
	RequestSchema{OpCode::ExportInfo, RequestDomain::Images, 2, s_exportId},
	RequestSchema{OpCode::ReleaseExport, RequestDomain::Images, 2, s_exportId},
	RequestSchema{OpCode::ReadExport, RequestDomain::Images, 2, s_readExport},
	RequestSchema{OpCode::RetainPreview, RequestDomain::Images, 3, s_preview},
	RequestSchema{OpCode::UploadSequence, RequestDomain::Simulation, 5, s_uploadSequence},
	RequestSchema{OpCode::StartSequence, RequestDomain::Simulation, 5, s_startSequence},
	RequestSchema{OpCode::ReadSequence, RequestDomain::Simulation, 5, s_sequence},
	RequestSchema{OpCode::StopSequence, RequestDomain::Simulation, 5, s_sequence},
	RequestSchema{OpCode::ReleaseSequenceCapture, RequestDomain::Inspection, 5, s_sequenceCapture},
	RequestSchema{OpCode::SubmitSimulation, RequestDomain::Simulation, 4, s_simulation},
	RequestSchema{OpCode::ReadHaptics, RequestDomain::Simulation, 6, s_common},
	RequestSchema{OpCode::SimulationStatus, RequestDomain::Simulation, 4, s_simulationStatus},
	RequestSchema{OpCode::CancelSimulation, RequestDomain::Simulation, 4, s_simulationStatus},
	RequestSchema{OpCode::ReleaseControl, RequestDomain::Simulation, 4, s_recording},
	RequestSchema{OpCode::StartRecording, RequestDomain::Simulation, 4, s_recording},
	RequestSchema{OpCode::StopRecording, RequestDomain::Simulation, 4, s_recording},
	RequestSchema{OpCode::ReadRecording, RequestDomain::Simulation, 4, s_recording},
	RequestSchema{OpCode::ReadDiagnostics, RequestDomain::Simulation, 4, s_diagnostics},
	RequestSchema{OpCode::GameStatus, RequestDomain::Game, 4, s_game},
	RequestSchema{OpCode::SubmitGame, RequestDomain::Game, 4, s_game},
	RequestSchema{OpCode::GameOperation, RequestDomain::Game, 4, s_game}};
std::uint64_t RequireRequestU64(const Message& request, std::uint16_t tag)
{
	const auto* field = request.Find(tag);
	const auto value = field ? field->AsU64() : std::nullopt;
	if (!value)
		throw ControlRequestError{ErrorCode::Invalid, "Missing or invalid uint64 field"};
	return *value;
}
std::uint32_t RequireRequestU32(const Message& request, std::uint16_t tag)
{
	const auto* field = request.Find(tag);
	const auto value = field ? field->AsU32() : std::nullopt;
	if (!value)
		throw ControlRequestError{ErrorCode::Invalid, "Missing or invalid uint32 field"};
	return *value;
}
const RequestSchema& ValidateControlRequest(const ControlPacket& packet, bool nativeHandles)
{
	const auto& request = packet.message;
	if (request.major != ProtocolMajor)
		throw ControlRequestError{ErrorCode::UnsupportedVersion, "Unsupported protocol major"};
	if (request.flags || !request.requestId.value)
		throw ControlRequestError{ErrorCode::Invalid, "Expected an identified request"};
	const auto schema = std::find_if(s_schemas.begin(), s_schemas.end(),
									 [&](const auto& entry)
									 {
										 return static_cast<std::uint16_t>(entry.opcode) == request.opcode;
									 });
	if (schema == s_schemas.end())
		throw ControlRequestError{ErrorCode::UnsupportedCommand, "Unsupported operation code"};
	for (const auto& field : request.fields)
		if (field.required && field.tag != tag::HandleCount &&
			std::find(schema->fields.begin(), schema->fields.end(), field.tag) == schema->fields.end())
			throw ControlRequestError{ErrorCode::UnsupportedField, "Unknown required request field"};
	std::uint32_t handleCount = 0;
	if (const auto field = request.Find(tag::HandleCount))
	{
		const auto value = field->AsU32();
		if (!value)
			throw ControlRequestError{ErrorCode::Invalid, "Missing or invalid uint32 field"};
		handleCount = *value;
	}
	const bool share = schema->opcode == OpCode::ShareImage;
	if (share && !nativeHandles)
		throw ControlRequestError{ErrorCode::UnsupportedCommand, "Transport does not support native image handles"};
	if (handleCount != packet.handles.size() || handleCount != (share ? 1U : 0U) ||
		(handleCount && (!nativeHandles || !packet.handles[0])))
		throw ControlRequestError{ErrorCode::Invalid, "Unexpected or missing native transfer handles"};
	return *schema;
}
} // namespace simxr::control
