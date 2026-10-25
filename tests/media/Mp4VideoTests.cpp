#include "../TestSupport.h"
#include "media/Mp4Video.h"
#include "presentation/FramePreviewHistory.h"
#include <array>
#include <iostream>

static std::uint32_t Number(std::span<const std::byte> data)
{
	std::uint32_t result = 0;
	for (auto value : data.first(4))
		result = result * 256 + std::to_integer<unsigned>(value);
	return result;
}
int main()
{
	try
	{
		// Minimal NAL units test the container contract, not decoder conformance.
		const std::array<std::uint8_t, 22> picture{0, 0,	0,	  1, 0x67, 77, 0, 31,	0x80, 0,	0,
												   1, 0x68, 0x80, 0, 0,	   0,  1, 0x65, 0x88, 0x84, 0x80};
		std::array<simxr::VideoSample, 2> samples{{{{picture, 256, 128}, 1000}, {{picture, 256, 128}, 2000}}};
		const auto bytes = simxr::WriteMp4Video(samples);
		std::size_t offset = 0;
		unsigned boxes = 0;
		while (offset < bytes.size())
		{
			const auto size = Number(std::span{bytes}.subspan(offset));
			Require(size >= 8 && size <= bytes.size() - offset, "Invalid MP4 box size");
			offset += size;
			++boxes;
		}
		Require(boxes == 3, "Expected ftyp, mdat and moov");
		samples[1].image.width = 128;
		bool rejected = false;
		try
		{
			simxr::WriteMp4Video(samples);
		}
		catch (const std::invalid_argument&)
		{
			rejected = true;
		}
		Require(rejected, "Dimension change must not silently corrupt a clip");
		samples[1] = samples[0];
		samples[1].duration = 0;
		rejected = false;
		try
		{
			simxr::WriteMp4Video(samples);
		}
		catch (const std::invalid_argument&)
		{
			rejected = true;
		}
		Require(rejected, "Invalid timing accepted");
		auto history = std::make_unique<simxr::FramePreviewHistory>();
		history->Append({{1}, {1}}, {}, true, nullptr, {simxr::Duration{100}}, simxr::Duration{10});
		history->Append({{1}, {2}}, {}, true, nullptr, {simxr::Duration{110}}, simxr::Duration{10});
		auto clip = history->RetainRange({{1}, {1}}, {{1}, {2}});
		Require(clip.size() == 2 && clip[1].time.value.count() == 110 && clip[0].period.count() == 10,
				"Clip lost frame timing");
		rejected = false;
		try
		{
			history->RetainRange({{1}, {1}}, {{1}, {3}});
		}
		catch (const std::runtime_error&)
		{
			rejected = true;
		}
		Require(rejected, "Missing frame must reject the complete clip");
		history.reset();
		Require(clip[0].retention && clip[0].clear, "Clip lost retention on history clear");
		std::cout << "PASS MP4 container bounds, timing and complete range retention\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
