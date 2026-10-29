#pragma once
#include "graphics/FramePool.h"
#include "media/H264Frame.h"
#include <cstdint>
#include <memory>

namespace simxr
{
class FrameOutput;

class ArchivedFrame
{
public:
	virtual ~ArchivedFrame() = default;

	virtual std::shared_ptr<const ArchivedFrame> Black() const
	{
		return {};
	}

	virtual H264Frame Video() const noexcept
	{
		return {};
	}

	virtual std::uint64_t Bytes() const noexcept = 0;
	// Worker-only operation; the returned GPU owner retains its budget token.
	virtual std::shared_ptr<const FrameOutput> Decode(const FramePool& budget) const = 0;
};
} // namespace simxr
