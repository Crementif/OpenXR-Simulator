#pragma once

#include "control/WireProtocol.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace simxr::control
{
inline constexpr std::size_t MaxTransferHandles = 4;
enum class TransferHandleKind : std::uint32_t
{
	OpaqueFd = 1,
	OpaqueWin32 = 2
};

// Platform code owns/closes native handles. The portable service never
// interprets their values, and a copied packet shares ownership safely.
class TransferHandle
{
  public:
	virtual ~TransferHandle() = default;
	virtual TransferHandleKind Kind() const noexcept = 0;
	virtual std::intptr_t Value() const noexcept = 0;
	virtual std::shared_ptr<TransferHandle> Duplicate() const = 0;
};

class PacketExposure
{
  public:
	virtual ~PacketExposure() = default;
	// The transport invokes this when native handles become visible, or before
	// an asynchronous send whose failed wait cannot rule out peer exposure.
	virtual void MarkTransferred() noexcept = 0;
};

struct ControlPacket
{
	Message message;
	std::vector<std::shared_ptr<TransferHandle>> handles;
	std::shared_ptr<PacketExposure> exposure;
	// Outgoing service packets prepare framing before committing publication.
	std::shared_ptr<const std::vector<std::byte>> encoded;
};
} // namespace simxr::control
