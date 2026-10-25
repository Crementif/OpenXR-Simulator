#pragma once

#include "control/ControlPacket.h"
#include "graphics/FrameOutput.h"
#include "graphics/SharedImageTypes.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <openxr/openxr.h>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace simxr::control
{
struct ImageCapabilities
{
	bool gpuPreview = false;
	bool imageExport = false;
	// Session support; individual retained images do not advertise this capability.
	bool rotationalReprojection = false;
};

struct ImageDeviceIdentity
{
	std::array<std::byte, 16> device{};
	std::array<std::byte, 16> driver{};
	auto operator<=>(const ImageDeviceIdentity&) const = default;
};

// Vulkan creation values remain explicit wire integers. The portable service
// copies metadata without including Vulkan or interpreting native handles.
struct ImageDescription
{
	ImageDeviceIdentity identity;
	std::uint32_t format = 0, width = 0, height = 0, depth = 1, arrayLayers = 0;
	std::uint32_t mipLevels = 1, samples = 1, usage = 0, tiling = 0, imageType = 1, flags = 0;
	std::uint64_t allocationSize = 0;
	std::uint32_t memoryTypeIndex = 0;
	bool dedicated = false;
	std::uint32_t producerQueueFamily = 0, layout = 0;
	std::uint32_t handleKind = 0;
	ImageCompletionKind completionKind = ImageCompletionKind::Fence;
	std::vector<FrameView> views;
};
Field EncodeImageDescription(const ImageDescription& description, bool external = false);
ImageDescription DecodeImageDescription(const Field& field);

class ImageInspectionError : public std::runtime_error
{
  public:
	ImageInspectionError(ErrorCode code, const char* message) : std::runtime_error(message), code(code) {}
	const ErrorCode code;
};

class ImageDelivery : public PacketExposure
{
  public:
	virtual SharedImageKey Key() const noexcept = 0;
	virtual ImageDescription Description() const = 0;
	virtual CompletionStatus ProducerCompletion() const noexcept = 0;
	virtual CompletionStatus ConsumerCompletion() const noexcept = 0;
	// Preparing descriptors does not expose the allocation. The transport marks
	// exposure once transfer can reach the peer (including an unfinished native send).
	virtual std::vector<std::shared_ptr<TransferHandle>> PrepareHandles() = 0;
};

class ImageExport : public GpuCompletion
{
  public:
	virtual std::string_view MediaType() const noexcept
	{
		return "image/raw";
	}
	virtual std::string Error() const
	{
		return {};
	}
	virtual ImageDescription Description() const = 0;
	virtual std::uint64_t ByteSize() const noexcept = 0;
	// Explicit export only; immutable borrowed storage lives through this ticket.
	virtual std::span<const std::byte> Read() = 0;
};

class ImageReader : public GpuCompletion
{
  public:
	virtual ImageDescription Description() const = 0;
	virtual ImageCapabilities Capabilities() const noexcept = 0;
	virtual std::shared_ptr<ImageDelivery> Share(const ImageDeviceIdentity& identity,
												 const std::shared_ptr<TransferHandle>& consumer) const = 0;
	virtual std::shared_ptr<ImageExport> Export(std::optional<std::uint32_t> layer) const = 0;
};

class ImageInspectionProvider
{
  public:
	virtual ~ImageInspectionProvider() = default;
	virtual ImageCapabilities Capabilities(XrSession session) const noexcept = 0;
	virtual std::shared_ptr<ImageReader> Retain(std::shared_ptr<const FrameOutput> output) const = 0;
};
} // namespace simxr::control
