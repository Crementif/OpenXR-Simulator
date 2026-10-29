#pragma once

#include <compare>
#include <cstdint>

namespace simxr
{
enum class ImageCompletionKind : std::uint32_t
{
	Fence = 0,
	Timeline = 1
};

// Pool generations and immutable allocations are separate identities. A newer
// pool does not invalidate readers retaining an older allocation.
struct ImagePoolGeneration
{
	std::uint64_t value = 0;
	auto operator<=>(const ImagePoolGeneration&) const = default;
};

struct SharedImageId
{
	std::uint64_t value = 0;
	auto operator<=>(const SharedImageId&) const = default;
};

struct SharedImageKey
{
	ImagePoolGeneration pool;
	SharedImageId image;
	auto operator<=>(const SharedImageKey&) const = default;
};
} // namespace simxr
