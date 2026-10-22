#include "control/ControlSchema.h"
#include "control/ControlPacket.h"
#include "control/ControlRequestError.h"

#include <algorithm>
#include <array>

namespace simxr::control
{
namespace
{
constexpr std::array uploadSequence{tag::Session, tag::Generation, tag::SourceKind, tag::ProgramId,
	tag::Offset, tag::TotalBytes, tag::Bytes};
constexpr std::array startSequence{tag::Session, tag::Generation, tag::SourceKind, tag::ProgramId, tag::Loop};
constexpr std::array sequence{tag::Session, tag::Generation, tag::SourceKind, tag::SequenceId, tag::Offset, tag::Length};
constexpr std::array sequenceCapture{tag::Session, tag::Generation, tag::CaptureId};
constexpr std::array simulation{tag::Session,	tag::Generation, tag::TransactionId, tag::ApplyFrame,
								tag::ApplyTime, tag::LatePolicy, tag::SourceKind,	 tag::Commands};
constexpr std::array simulationStatus{tag::Session, tag::Generation, tag::TransactionId, tag::SourceKind};
constexpr std::array game{tag::Session,	  tag::Generation,	tag::GameId, tag::GameKind,
						  tag::CommandId, tag::OperationId, tag::Detail};
constexpr std::array diagnostics{tag::Session, tag::Generation, tag::Offset, tag::Length, tag::FrameId};
constexpr std::array historicalImage{tag::Session, tag::Generation, tag::FrameId, tag::Target};
constexpr std::array imageHistory{tag::Session, tag::Generation, tag::Length, tag::Target};
constexpr std::array beginImageWindow{tag::Session, tag::Generation, tag::Target};
constexpr std::array frameHistory{tag::Session, tag::Generation, tag::Offset, tag::Length};
constexpr std::array projectionHistory{tag::Session, tag::Generation, tag::Length};
constexpr std::array recording{tag::Session, tag::Generation, tag::SourceKind, tag::Offset, tag::Length};
constexpr std::array common{tag::Session, tag::Generation};
constexpr std::array hello{tag::ClientName, tag::RequiredCapabilities, tag::OptionalCapabilities};
constexpr std::array submit{tag::Session,	tag::Generation, tag::Lease,	   tag::CommandId, tag::CommandKind,
							tag::Lookahead, tag::Target,	 tag::OperationId, tag::CaptureId, tag::Capacity, tag::Length, tag::AdvanceDisplay, tag::SnapshotBudgetMiB, tag::AdditionalTargets};
constexpr std::array leased{tag::Session, tag::Generation, tag::Lease};
constexpr std::array operation{tag::Session, tag::Generation, tag::OperationId};
constexpr std::array acknowledge{tag::Session, tag::Generation, tag::Lease, tag::CommandId};
constexpr std::array capture{tag::Session, tag::Generation, tag::CaptureId};
constexpr std::array retainImage{tag::Session, tag::Generation, tag::CaptureId, tag::Target};
constexpr std::array revision{tag::RevisionId};
constexpr std::array releaseInspection{tag::InspectionId};
constexpr std::array imageWindow{tag::ImageWindowId};
constexpr std::array windowImage{tag::ImageWindowId, tag::FrameId};
constexpr std::array image{tag::ImageReaderId};
constexpr std::array share{tag::ImageReaderId, tag::DeviceUuid, tag::DriverUuid, tag::CompletionKind};
constexpr std::array delivery{tag::DeliveryId};
constexpr std::array startExport{tag::ImageReaderId, tag::Layer};
constexpr std::array exportId{tag::ExportId};
constexpr std::array readExport{tag::ExportId, tag::Offset, tag::Length};
constexpr std::array Schemas{RequestSchema{OpCode::Hello, RequestDomain::Connection, 0, hello},
							 RequestSchema{OpCode::Sessions, RequestDomain::Connection, 0, {}},
							 RequestSchema{OpCode::Status, RequestDomain::Debugger, 0, common},
							 RequestSchema{OpCode::Acquire, RequestDomain::Debugger, 0, common},
							 RequestSchema{OpCode::Renew, RequestDomain::Debugger, 0, leased},
							 RequestSchema{OpCode::Relinquish, RequestDomain::Debugger, 0, leased},
							 RequestSchema{OpCode::Submit, RequestDomain::Debugger, 0, submit},
							 RequestSchema{OpCode::Operation, RequestDomain::Debugger, 0, operation},
							 RequestSchema{OpCode::Acknowledge, RequestDomain::Debugger, 0, acknowledge},
							 RequestSchema{OpCode::Capture, RequestDomain::Inspection, 0, capture},
							 RequestSchema{OpCode::InspectFrame, RequestDomain::Inspection, 0, capture},
							 RequestSchema{OpCode::ReadFrameHistory, RequestDomain::Inspection, 4, frameHistory},
							 RequestSchema{OpCode::ReadProjectionHistory, RequestDomain::Inspection, 4, projectionHistory},
							 RequestSchema{OpCode::BeginImageWindow, RequestDomain::Inspection, 4, beginImageWindow},
							 RequestSchema{OpCode::ReadImageWindow, RequestDomain::Images, 4, imageWindow},
							 RequestSchema{OpCode::ReleaseImageWindow, RequestDomain::Images, 4, imageWindow},
							 RequestSchema{OpCode::RetainWindowImage, RequestDomain::Images, 4, windowImage},
							 RequestSchema{OpCode::ReadImageHistory, RequestDomain::Inspection, 4, imageHistory},
							 RequestSchema{OpCode::RetainHistoricalImage, RequestDomain::Inspection, 4, historicalImage},
							 RequestSchema{OpCode::FeatureRevision, RequestDomain::Inspection, 0, revision},
							 RequestSchema{OpCode::ReleaseInspection, RequestDomain::Inspection, 0, releaseInspection},
							 RequestSchema{OpCode::RetainImage, RequestDomain::Images, 2, retainImage},
							 RequestSchema{OpCode::ImageInfo, RequestDomain::Images, 2, image},
							 RequestSchema{OpCode::ReleaseImage, RequestDomain::Images, 2, image},
							 RequestSchema{OpCode::ShareImage, RequestDomain::Images, 2, share},
							 RequestSchema{OpCode::DeliveryInfo, RequestDomain::Images, 2, delivery},
							 RequestSchema{OpCode::ReleaseDelivery, RequestDomain::Images, 2, delivery},
							 RequestSchema{OpCode::StartExport, RequestDomain::Images, 2, startExport},
							 RequestSchema{OpCode::ExportInfo, RequestDomain::Images, 2, exportId},
							 RequestSchema{OpCode::ReleaseExport, RequestDomain::Images, 2, exportId},
							 RequestSchema{OpCode::ReadExport, RequestDomain::Images, 2, readExport},
							 RequestSchema{OpCode::RetainPreview, RequestDomain::Images, 3, common},
							 RequestSchema{OpCode::UploadSequence, RequestDomain::Simulation, 5, uploadSequence},
							 RequestSchema{OpCode::StartSequence, RequestDomain::Simulation, 5, startSequence},
							 RequestSchema{OpCode::ReadSequence, RequestDomain::Simulation, 5, sequence},
							 RequestSchema{OpCode::StopSequence, RequestDomain::Simulation, 5, sequence},
							 RequestSchema{OpCode::ReleaseSequenceCapture, RequestDomain::Inspection, 5, sequenceCapture},
							 RequestSchema{OpCode::SubmitSimulation, RequestDomain::Simulation, 4, simulation},
							 RequestSchema{OpCode::SimulationStatus, RequestDomain::Simulation, 4, simulationStatus},
							 RequestSchema{OpCode::CancelSimulation, RequestDomain::Simulation, 4, simulationStatus},
							 RequestSchema{OpCode::ReleaseControl, RequestDomain::Simulation, 4, recording},
							 RequestSchema{OpCode::StartRecording, RequestDomain::Simulation, 4, recording},
							 RequestSchema{OpCode::StopRecording, RequestDomain::Simulation, 4, recording},
							 RequestSchema{OpCode::ReadRecording, RequestDomain::Simulation, 4, recording},
							 RequestSchema{OpCode::ReadDiagnostics, RequestDomain::Simulation, 4, diagnostics},
							 RequestSchema{OpCode::GameStatus, RequestDomain::Game, 4, game},
							 RequestSchema{OpCode::SubmitGame, RequestDomain::Game, 4, game},
							 RequestSchema{OpCode::GameOperation, RequestDomain::Game, 4, game}};
} // namespace
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
	const auto schema = std::find_if(Schemas.begin(), Schemas.end(),
									 [&](const auto& entry)
									 {
										 return static_cast<std::uint16_t>(entry.opcode) == request.opcode;
									 });
	if (schema == Schemas.end())
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
