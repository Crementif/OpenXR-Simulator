#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace simxr::app::mcp
{
// Bounds aggregate memory and encoding work independently of HTTP worker count.
class ImageEncodingPermit
{
  public:
	ImageEncodingPermit();
	~ImageEncodingPermit();
	ImageEncodingPermit(const ImageEncodingPermit&) = delete;
	ImageEncodingPermit& operator=(const ImageEncodingPermit&) = delete;
};

inline constexpr std::uint64_t MaxImageInput = 32ULL * 1024 * 1024;
inline constexpr std::size_t MaxEncodedImage = 8ULL * 1024 * 1024;

struct EncodedImage
{
	std::vector<std::byte> bytes;
	std::string mediaType;
	bool lossy = false;
};
EncodedImage EncodeImage(std::span<const std::byte> pixels, std::uint32_t format, std::uint32_t width,
						 std::uint32_t height, bool png, int quality);
} // namespace simxr::app::mcp
