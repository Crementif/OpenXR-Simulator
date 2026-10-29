#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace simxr::app
{
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

struct ImageEncodingLimits
{
	std::uint32_t dimension = 4096;
	std::uint64_t inputBytes = MaxImageInput;
	std::size_t outputBytes = MaxEncodedImage;
};
inline constexpr ImageEncodingLimits FileImageLimits{16384, 512ULL * 1024 * 1024, 512ULL * 1024 * 1024};

struct EncodedImage
{
	std::vector<std::byte> bytes;
	std::string mediaType;
	bool lossy = false;
};
EncodedImage EncodeImage(std::span<const std::byte> pixels, std::uint32_t format, std::uint32_t width,
						 std::uint32_t height, bool png, int quality, ImageEncodingLimits limits = {});
void SaveExportFile(const std::filesystem::path& destination, std::span<const std::byte> bytes,
	bool overwrite, const std::function<void()>& beforeCommit = {});
void SaveExportFile(const std::filesystem::path& destination, const std::function<void(std::ostream&)>& write,
	bool overwrite, const std::function<void()>& beforeCommit = {});
} // namespace simxr::app
