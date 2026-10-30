#include "Pch.h"
#include "app/mcp/McpImage.h"
#include "app/mcp/McpImageCache.h"
#include "app/services/ImageExport.h"
#include "app/mcp/McpStereoAnalysis.h"
#include "control/ControlSchema.h"
#include "graphics/ColorPixels.h"
#include "../TestSupport.h"

#include <latch>

int main(int argc, char** argv)
{
	using namespace simxr;
	using namespace simxr::app;
	using namespace simxr::app::mcp;
	Require(CacheableImageSource(static_cast<unsigned>(control::TargetKind::LayerView), false, false),
			"A captured layer without a display tick can be reused");
	Require(!CacheableImageSource(static_cast<unsigned>(control::TargetKind::VirtualDisplay), false, false) &&
				CacheableImageSource(static_cast<unsigned>(control::TargetKind::VirtualDisplay), true, true),
			"A virtual display needs its own image tick for cross-export reuse");
	Require(!CacheableImageSource(static_cast<unsigned>(control::TargetKind::ComposedEyes), true, false),
			"An invalid image tick must not be cached");
	{
		constexpr std::uint32_t width = 128, height = 96;
		std::vector<std::uint8_t> left(width * height), right(width * height), flat(width * height, 20);
		std::uint32_t state = 0x12345678;
		for (auto& pixel : left)
		{
			state = state * 1664525 + 1013904223;
			pixel = static_cast<std::uint8_t>(state >> 24);
		}
		for (std::uint32_t y = 0; y < height; ++y)
			for (std::uint32_t x = 4; x < width; ++x)
				right[y * width + x] = left[y * width + x - 4];
		const auto check = [] {
		};
		const auto shifted = AnalyzeStereo(left, right, width, height, width, check);
		Require(shifted.at("verdict") == "PASS" && shifted.at("horizontal_disparity_px") == 4,
				"Stereo analysis did not recover the known eye shift");
		Require(AnalyzeStereo(left, left, width, height, width, check).at("verdict") == "FAIL_NO_PARALLAX",
				"Stereo analysis did not identify identical textured eyes");
		Require(AnalyzeStereo(flat, flat, width, height, width, check).at("verdict") == "INCONCLUSIVE_NO_FEATURES",
				"Featureless images should not imply a stereo verdict");
		for (std::uint32_t y = 0; y < height; ++y)
			for (std::uint32_t x = 0; x < width; ++x)
				flat[y * width + x] = (x % 8 < 4) ? 0 : 255;
		Require(AnalyzeStereo(flat, flat, width, height, width, check).at("verdict") == "INCONCLUSIVE_AMBIGUOUS",
				"Repeated patterns should not imply a unique stereo match");
		bool interrupted = false;
		try
		{
			(void)AnalyzeStereo(left, right, width, height, width, [] { throw std::runtime_error("cancelled"); });
		}
		catch (const std::runtime_error&)
		{
			interrupted = true;
		}
		Require(interrupted, "Stereo analysis ignored cancellation");
	}
	{
		ImageCache cache(30, 2);
		const auto makeImage = [](std::string data) {
			auto image = std::make_shared<CachedImage>();
			image->data = std::move(data);
			image->mediaType = "png";
			return image;
		};
		cache.Insert("first", makeImage("one"));
		cache.Insert("second", makeImage("two"));
		const auto retained = cache.Find("first");
		Require(retained && retained->data == "one", "Image cache did not preserve its content");
		cache.Insert("third", makeImage("three"));
		Require(!cache.Find("second") && cache.Find("first") && cache.Find("third"),
				"Image cache did not evict least recently used content");
		cache.Insert("huge", makeImage(std::string(30, 'x')));
		Require(!cache.Find("huge") && cache.Find("first"), "Oversized content displaced the bounded cache");
		cache.Insert("fourth", makeImage("four"));
		cache.Insert("fifth", makeImage("five"));
		Require(retained->data == "one" && !cache.Find("first"), "An evicted image did not survive an active reader");
		ImageCache byteBound(15, 8);
		byteBound.Insert("a", makeImage("12345"));
		byteBound.Insert("b", makeImage("12345"));
		Require(!byteBound.Find("a") && byteBound.Find("b"), "Image cache exceeded its byte budget");
	}
	{
		ImageCache cache(1024, 1);
		unsigned retired = 0;
		const auto image = [&] {
			return std::shared_ptr<const CachedImage>(new CachedImage{"pixels", "png"}, [&](const CachedImage* value) {
				// Destruction must be able to reenter the cache after publication.
				(void)cache.Find("replacement");
				++retired;
				delete value;
			});
		};
		cache.Insert("original", image());
		cache.Insert("replacement", std::make_shared<CachedImage>());
		Require(retired == 1, "Cache eviction retained discarded storage");
		cache.Insert("replacement", image());
		Require(retired == 2, "Duplicate image storage was not released outside the cache lock");
	}

	{
		ImageEncodingPermit first;
		ImageEncodingPermit second;
		bool rejected = false;
		try
		{
			ImageEncodingPermit third;
		}
		catch (const std::runtime_error&)
		{
			rejected = true;
		}
		Require(rejected, "A third concurrent image encoding exceeded its budget");
	}
	const std::vector<std::byte> rgba(64 * 64 * 4, std::byte{0x7f});
	EncodedImage png, jpeg;
	ImageEncodingPermit firstPermit;
	ImageEncodingPermit secondPermit;
	std::exception_ptr firstError, secondError;
	std::latch start(1);
	std::jthread first([&] {
		try
		{
			start.wait();
			png = EncodeImage(rgba, static_cast<std::uint32_t>(ColorFormat::Rgba8Unorm), 64, 64, true, 85);
		}
		catch (...)
		{
			firstError = std::current_exception();
		}
	});
	std::jthread second;
	try
	{
		second = std::jthread([&] {
			try
			{
				start.wait();
				jpeg = EncodeImage(rgba, static_cast<std::uint32_t>(ColorFormat::Rgba8Unorm), 64, 64, false, 85);
			}
			catch (...)
			{
				secondError = std::current_exception();
			}
		});
	}
	catch (...)
	{
		start.count_down();
		throw;
	}
	start.count_down();
	first.join();
	second.join();
	if (firstError)
		std::rethrow_exception(firstError);
	if (secondError)
		std::rethrow_exception(secondError);
	Require(png.mediaType == "image/png" && !png.lossy && png.bytes.size() > 8 && png.bytes[0] == std::byte{0x89} &&
				png.bytes[1] == std::byte{0x50},
			"Concurrent PNG encoding failed");
	Require(jpeg.mediaType == "image/jpeg" && jpeg.lossy && jpeg.bytes.size() > 4 && jpeg.bytes[0] == std::byte{0xff} &&
				jpeg.bytes[1] == std::byte{0xd8},
			"Concurrent JPEG encoding failed");
	std::vector<std::byte> floating(8 * sizeof(float));
	const std::array values{-1.0f,
							0.5f,
							2.0f,
							1.0f,
							std::numeric_limits<float>::quiet_NaN(),
							std::numeric_limits<float>::infinity(),
							-std::numeric_limits<float>::infinity(),
							1.0f};
	std::memcpy(floating.data(), values.data(), floating.size());
	const auto hdr = EncodeImage(floating, static_cast<std::uint32_t>(ColorFormat::Rgba32Float), 2, 1, true, 85);
	Require(hdr.lossy && !hdr.bytes.empty(), "HDR to PNG conversion did not report lost precision");
	const std::uint32_t packed = (512U << 10) | (1023U << 20) | (3U << 30);
	std::array<std::byte, 4> tenBit{};
	for (std::size_t index = 0; index < tenBit.size(); ++index)
		tenBit[index] = static_cast<std::byte>((packed >> (index * 8)) & 0xff);
	const auto rgb10 = EncodeImage(tenBit, static_cast<std::uint32_t>(ColorFormat::Rgb10A2Unorm), 1, 1, true, 85);
	Require(rgb10.lossy && !rgb10.bytes.empty(), "10-bit to PNG conversion did not report lost precision");
	const std::array bgra{std::byte{0x31}, std::byte{255}, std::byte{0}, std::byte{128}};
	const auto swizzled = EncodeImage(bgra, static_cast<std::uint32_t>(ColorFormat::Bgra8Unorm), 1, 1, true, 85);
	const std::array half{std::byte{0}, std::byte{0xb8}, std::byte{0}, std::byte{0x38},
						  std::byte{0}, std::byte{0x40}, std::byte{0}, std::byte{0x34}};
	const auto hdr16 = EncodeImage(half, static_cast<std::uint32_t>(ColorFormat::Rgba16Float), 1, 1, true, 85);
	if (argc == 2 && std::string_view(argv[1]) == "--fixtures")
	{
		std::printf("HDR:%s\nRGB10:%s\n", EncodeBase64(hdr.bytes).c_str(), EncodeBase64(rgb10.bytes).c_str());
		std::printf("BGRA:%s\nHDR16:%s\n", EncodeBase64(swizzled.bytes).c_str(), EncodeBase64(hdr16.bytes).c_str());
	}
	std::array<std::byte, 256 * 4> byteColors{};
	for (unsigned index = 0; index < 256; ++index)
	{
		byteColors[index * 4] = std::byte(index);
		byteColors[index * 4 + 1] = std::byte(255 - index);
		byteColors[index * 4 + 2] = std::byte((index * 73) & 255);
		byteColors[index * 4 + 3] = std::byte((index * 31) & 255);
	}
	const auto reference =
		EncodeImage(byteColors, static_cast<std::uint32_t>(ColorFormat::Rgba8Unorm), 256, 1, false, 85);
	for (const auto format :
		 {ColorFormat::Rgba8Unorm, ColorFormat::Rgba8Srgb, ColorFormat::Bgra8Unorm, ColorFormat::Bgra8Srgb})
	{
		auto input = byteColors;
		if (format == ColorFormat::Bgra8Unorm || format == ColorFormat::Bgra8Srgb)
			for (unsigned index = 0; index < 256; ++index)
				std::swap(input[index * 4], input[index * 4 + 2]);
		const auto encoded = EncodeImage(input, static_cast<std::uint32_t>(format), 256, 1, true, 85);
		Require(!encoded.lossy, "Eight-bit PNG lost precision");
		if (argc == 2 && std::string_view(argv[1]) == "--fixtures")
			std::printf("BYTES%u:%s\n", static_cast<unsigned>(format), EncodeBase64(encoded.bytes).c_str());
		for (unsigned index = 0; index < 256; ++index)
			input[index * 4 + 3] = std::byte{255};
		Require(EncodeImage(input, static_cast<std::uint32_t>(format), 256, 1, false, 85).bytes == reference.bytes,
				"JPEG changed with byte channel layout, sRGB flag or alpha");
	}
	bool rejected = false;
	try
	{
		(void)EncodeImage(rgba, static_cast<std::uint32_t>(ColorFormat::Rgba8Unorm), 4097, 1, true, 85);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Oversized image reached the encoder");
	return 0;
}
