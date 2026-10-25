#include "media/Mp4Video.h"
#include "Pch.h"
#include <limits>
#include <stdexcept>

namespace simxr
{
class Mp4Boxes
{
  public:
	std::vector<std::byte> bytes;
	void Number(std::uint64_t value, unsigned count = 4)
	{
		for (unsigned i = count; i; --i)
			bytes.push_back(static_cast<std::byte>(value >> ((i - 1) * 8)));
	}
	void Text(std::string_view text)
	{
		for (auto c : text)
			bytes.push_back(static_cast<std::byte>(c));
	}
	void Zero(std::size_t count)
	{
		bytes.insert(bytes.end(), count, std::byte{});
	}
	void Data(std::span<const std::uint8_t> data)
	{
		const auto raw = std::as_bytes(data);
		bytes.insert(bytes.end(), raw.begin(), raw.end());
	}
	std::size_t Begin(std::string_view type)
	{
		const auto offset = bytes.size();
		Number(0);
		Text(type);
		return offset;
	}
	void End(std::size_t offset)
	{
		const auto size = bytes.size() - offset;
		if (size > UINT32_MAX)
			throw std::length_error("Video box is too large.");
		for (unsigned i = 0; i < 4; ++i)
			bytes[offset + i] = static_cast<std::byte>(size >> ((3 - i) * 8));
	}
	void Matrix()
	{
		for (auto value : {0x10000u, 0u, 0u, 0u, 0x10000u, 0u, 0u, 0u, 0x40000000u})
			Number(value);
	}
};
static std::vector<std::span<const std::uint8_t>> NalUnits(std::span<const std::uint8_t> data)
{
	std::vector<std::span<const std::uint8_t>> result;
	std::size_t start = 0, zeros = 0;
	bool found = false;
	for (std::size_t i = 0; i < data.size(); ++i)
	{
		if (!data[i])
		{
			++zeros;
			continue;
		}
		if (data[i] == 1 && zeros >= 2)
		{
			if (found && i - zeros > start)
				result.push_back(data.subspan(start, i - zeros - start));
			start = i + 1;
			found = true;
		}
		zeros = 0;
	}
	if (found && data.size() - zeros > start)
		result.push_back(data.subspan(start, data.size() - zeros - start));
	if (result.empty())
		throw std::invalid_argument("The clip contains invalid H.264 data.");
	return result;
}
std::vector<std::byte> WriteMp4Video(std::span<const VideoSample> samples)
{
	if (samples.empty() || samples.size() > 1000000)
		throw std::invalid_argument("Choose a nonempty clip of at most one million frames.");
	const auto width = samples.front().image.width, height = samples.front().image.height;
	if (!width || !height || width > 65535 || height > 65535)
		throw std::invalid_argument("The video dimensions are unsupported.");
	Mp4Boxes out;
	auto box = out.Begin("ftyp");
	out.Text("isom");
	out.Number(512);
	out.Text("isomiso2avc1mp41");
	out.End(box);
	const auto mdat = out.Begin("mdat");
	std::vector<std::uint32_t> sizes, offsets;
	std::vector<std::uint8_t> sps, pps;
	std::uint64_t duration = 0;
	for (const auto& sample : samples)
	{
		if (sample.image.width != width || sample.image.height != height || !sample.duration)
			throw std::invalid_argument("The clip changes image size or has invalid frame timing.");
		duration += sample.duration;
		if (duration > UINT32_MAX)
			throw std::length_error("This clip is too long. Save a shorter range.");
		offsets.push_back(static_cast<std::uint32_t>(out.bytes.size()));
		bool idr = false;
		for (const auto nal : NalUnits(sample.image.bytes))
		{
			const auto type = nal.front() & 31;
			if (type == 7 || type == 8)
			{
				auto& parameters = type == 7 ? sps : pps;
				if (parameters.empty())
					parameters.assign(nal.begin(), nal.end());
				else if (!std::equal(parameters.begin(), parameters.end(), nal.begin(), nal.end()))
					throw std::invalid_argument("The video format changes in this clip. Save each part separately.");
				continue;
			}
			idr |= type == 5;
			if (out.bytes.size() + nal.size() + 4 > (480ull << 20))
				throw std::length_error("The video exceeds 480 MiB. Save a shorter range.");
			out.Number(nal.size());
			out.Data(nal);
		}
		if (!idr)
			throw std::invalid_argument("The clip contains a frame that is not independently decodable.");
		sizes.push_back(static_cast<std::uint32_t>(out.bytes.size() - offsets.back()));
	}
	if (sps.size() < 4 || sps.size() > 65535 || pps.empty() || pps.size() > 65535)
		throw std::invalid_argument("The clip has invalid H.264 parameters.");
	out.End(mdat);
	const auto moov = out.Begin("moov");
	box = out.Begin("mvhd");
	out.Zero(12);
	out.Number(90000);
	out.Number(duration);
	out.Number(0x10000);
	out.Number(0x100, 2);
	out.Zero(10);
	out.Matrix();
	out.Zero(24);
	out.Number(2);
	out.End(box);
	const auto trak = out.Begin("trak");
	box = out.Begin("tkhd");
	out.Number(7);
	out.Zero(8);
	out.Number(1);
	out.Zero(4);
	out.Number(duration);
	out.Zero(16);
	out.Matrix();
	out.Number(width << 16);
	out.Number(height << 16);
	out.End(box);
	const auto mdia = out.Begin("mdia");
	box = out.Begin("mdhd");
	out.Zero(12);
	out.Number(90000);
	out.Number(duration);
	out.Number(0x55c4, 2);
	out.Zero(2);
	out.End(box);
	box = out.Begin("hdlr");
	out.Zero(8);
	out.Text("vide");
	out.Zero(12);
	out.Text("Video");
	out.Zero(1);
	out.End(box);
	const auto minf = out.Begin("minf");
	box = out.Begin("vmhd");
	out.Number(1);
	out.Zero(8);
	out.End(box);
	const auto dinf = out.Begin("dinf"), dref = out.Begin("dref");
	out.Zero(4);
	out.Number(1);
	box = out.Begin("url ");
	out.Number(1);
	out.End(box);
	out.End(dref);
	out.End(dinf);
	const auto stbl = out.Begin("stbl"), stsd = out.Begin("stsd");
	out.Zero(4);
	out.Number(1);
	const auto avc1 = out.Begin("avc1");
	out.Zero(6);
	out.Number(1, 2);
	out.Zero(16);
	out.Number(width, 2);
	out.Number(height, 2);
	out.Number(0x480000);
	out.Number(0x480000);
	out.Zero(4);
	out.Number(1, 2);
	out.Zero(32);
	out.Number(24, 2);
	out.Number(65535, 2);
	box = out.Begin("avcC");
	out.Number(1, 1);
	out.Data(std::span{sps}.subspan(1, 3));
	out.Number(255, 1);
	out.Number(225, 1);
	out.Number(sps.size(), 2);
	out.Data(sps);
	out.Number(1, 1);
	out.Number(pps.size(), 2);
	out.Data(pps);
	out.End(box);
	out.End(avc1);
	out.End(stsd);
	box = out.Begin("stts");
	out.Zero(4);
	out.Number(samples.size());
	for (const auto& sample : samples)
	{
		out.Number(1);
		out.Number(sample.duration);
	}
	out.End(box);
	box = out.Begin("stsc");
	out.Zero(4);
	out.Number(1);
	out.Number(1);
	out.Number(1);
	out.Number(1);
	out.End(box);
	box = out.Begin("stsz");
	out.Zero(8);
	out.Number(sizes.size());
	for (auto size : sizes)
		out.Number(size);
	out.End(box);
	box = out.Begin("stco");
	out.Zero(4);
	out.Number(offsets.size());
	for (auto offset : offsets)
		out.Number(offset);
	out.End(box);
	out.End(stbl);
	out.End(minf);
	out.End(mdia);
	out.End(trak);
	out.End(moov);
	return std::move(out.bytes);
}
} // namespace simxr
