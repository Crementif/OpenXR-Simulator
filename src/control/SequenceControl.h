#pragma once

#include "control/InspectionDocument.h"
#include "control/ProtocolTypes.h"
#include "presentation/SnapshotStore.h"
#include "simulation/PoseSequenceExecution.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace simxr::control
{
struct ControlSession;
struct Message;
bool ApplicationOwnedSequence(const Message& request);

// One bounded upload per connection. The enclosing simulation owner serializes
// requests and retires every admitted execution on disconnect.
class SequenceControl
{
public:
	static constexpr std::size_t MaxBytes = 16 * 1024 * 1024;
	static constexpr std::size_t ChunkBytes = 256 * 1024;

	InspectionValue Dispatch(const Message& request, const ControlSession& binding, CommandSource source,
							 Message& response, std::vector<SnapshotRecord>& held);

private:
	InspectionValue Upload(const Message& request, const ControlSession& binding);
	InspectionValue Start(const Message& request, const ControlSession& binding, CommandSource source,
						  Message& response);
	InspectionValue Read(const Message& request, const ControlSession& binding, CommandSource source,
						 std::vector<SnapshotRecord>& held);

	std::uint64_t m_program = 0;
	ControlSessionId m_session;
	SessionGeneration m_generation;
	std::size_t m_total = 0;
	std::string m_bytes;
	std::optional<std::uint64_t> m_started;
	bool m_loop = false;
};
} // namespace simxr::control
