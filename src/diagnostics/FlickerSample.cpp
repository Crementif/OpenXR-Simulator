#include "diagnostics/FlickerSample.h"
#include "Pch.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace flicker
{
static uint8_t Luma(const uint8_t* bgra)
{
	return static_cast<uint8_t>((19u * bgra[0] + 183u * bgra[1] + 54u * bgra[2]) >> 8);
}

class EyeBrightness
{
  public:
	void Observe(uint8_t luma)
	{
		m_sum += luma / 255.0;
		++m_count;
		if (luma >= 245)
			++m_brightCount;
	}
	float Mean() const
	{
		return m_count ? static_cast<float>(m_sum / m_count) : 0.0f;
	}
	float BrightFraction() const
	{
		return m_count ? static_cast<float>(m_brightCount) / m_count : 0.0f;
	}

  private:
	double m_sum = 0.0;
	uint64_t m_count = 0;
	uint64_t m_brightCount = 0;
};

bool ValidPixels(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitchBytes)
{
	if (!bgra || width < 4 || height < 2 || width > std::numeric_limits<uint32_t>::max() / 4)
		return false;
	const auto stride = pitchBytes ? pitchBytes : width * 4;
	return stride >= width * 4 && height <= std::numeric_limits<size_t>::max() / stride;
}

Sample Downsample(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitchBytes, uint64_t frame)
{
	Sample sample;
	sample.frame = frame;
	sample.width = std::min(width, 320u);
	const auto scaledHeight = std::llround(static_cast<double>(height) * sample.width / width);
	sample.height = static_cast<uint32_t>(std::clamp<int64_t>(scaledHeight, 1, 180));
	auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(sample.width) * sample.height * 4);
	std::array<size_t, 320> sourceOffsets{};
	for (uint32_t x = 0; x < sample.width; ++x)
		sourceOffsets[x] = static_cast<size_t>(static_cast<uint64_t>(x) * width / sample.width) * 4;

	std::array<EyeBrightness, 2> eyes;
	double sum = 0.0;
	for (uint32_t y = 0; y < sample.height; ++y)
	{
		const auto sourceY = static_cast<uint32_t>(static_cast<uint64_t>(y) * height / sample.height);
		const auto* sourceRow = bgra + static_cast<size_t>(sourceY) * pitchBytes;
		auto* destinationRow = pixels->data() + static_cast<size_t>(y) * sample.width * 4;
		for (uint32_t x = 0; x < sample.width; ++x)
		{
			const auto* source = sourceRow + sourceOffsets[x];
			std::memcpy(destinationRow + static_cast<size_t>(x) * 4, source, 4);
			const auto luma = Luma(source);
			sum += luma / 255.0;
			eyes[x < sample.width / 2 ? 0 : 1].Observe(luma);
		}
	}
	sample.metrics.mean = static_cast<float>(sum / (static_cast<uint64_t>(sample.width) * sample.height));
	sample.metrics.meanLeft = eyes[0].Mean();
	sample.metrics.meanRight = eyes[1].Mean();
	sample.metrics.brightFractionLeft = eyes[0].BrightFraction();
	sample.metrics.brightFractionRight = eyes[1].BrightFraction();
	sample.pixels = std::move(pixels);
	return sample;
}

Sample CropUi(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitchBytes, const int32_t rects[2][4],
			  uint64_t frame)
{
	Sample sample;
	sample.frame = frame;
	sample.width = 256;
	sample.height = 72;
	auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(sample.width) * sample.height * 4, 0);
	std::array<EyeBrightness, 2> eyes;
	for (size_t eye = 0; eye < eyes.size(); ++eye)
	{
		const auto left = std::clamp<int64_t>(rects[eye][0], 0, width);
		const auto top = std::clamp<int64_t>(rects[eye][1], 0, height);
		const auto right = std::clamp<int64_t>(static_cast<int64_t>(rects[eye][0]) + rects[eye][2], 0, width);
		const auto bottom = std::clamp<int64_t>(static_cast<int64_t>(rects[eye][1]) + rects[eye][3], 0, height);
		if (right <= left || bottom <= top)
			continue;
		std::array<size_t, 128> sourceOffsets{};
		for (uint32_t x = 0; x < sourceOffsets.size(); ++x)
			sourceOffsets[x] = static_cast<size_t>(left + x * (right - left) / sourceOffsets.size()) * 4;
		for (uint32_t y = 0; y < sample.height; ++y)
		{
			const auto sourceY = static_cast<size_t>(top + y * (bottom - top) / sample.height);
			const auto* sourceRow = bgra + sourceY * pitchBytes;
			auto* destinationRow = pixels->data() + (static_cast<size_t>(y) * sample.width + eye * 128) * 4;
			for (size_t x = 0; x < sourceOffsets.size(); ++x)
			{
				const auto* source = sourceRow + sourceOffsets[x];
				std::memcpy(destinationRow + x * 4, source, 4);
				eyes[eye].Observe(Luma(source));
			}
		}
	}
	sample.metrics.meanLeft = eyes[0].Mean();
	sample.metrics.meanRight = eyes[1].Mean();
	sample.metrics.mean = (sample.metrics.meanLeft + sample.metrics.meanRight) * 0.5f;
	sample.pixels = std::move(pixels);
	return sample;
}

void CalculateTemporal(Sample& current, const Sample& previous)
{
	if (!current.pixels || !previous.pixels || current.width != previous.width || current.height != previous.height ||
		current.pixels->size() != previous.pixels->size())
		return;
	double leftDelta = 0.0;
	double rightDelta = 0.0;
	for (uint32_t y = 0; y < current.height; ++y)
	{
		for (uint32_t x = 0; x < current.width; ++x)
		{
			const auto offset = (static_cast<size_t>(y) * current.width + x) * 4;
			const auto delta = std::abs(static_cast<int>(Luma(current.pixels->data() + offset)) -
										static_cast<int>(Luma(previous.pixels->data() + offset))) /
							   255.0;
			if (x < current.width / 2)
				leftDelta += delta;
			else
				rightDelta += delta;
		}
	}
	const auto leftCount = static_cast<uint64_t>(current.width / 2) * current.height;
	const auto rightCount = static_cast<uint64_t>(current.width - current.width / 2) * current.height;
	current.metrics.temporal = static_cast<float>((leftDelta + rightDelta) / (leftCount + rightCount));
	current.metrics.temporalLeft = static_cast<float>(leftDelta / leftCount);
	current.metrics.temporalRight = static_cast<float>(rightDelta / rightCount);
}

float TemporalBetween(const Sample& first, const Sample& second)
{
	if (!first.pixels || !second.pixels || first.width != second.width || first.height != second.height ||
		first.pixels->size() != second.pixels->size() || first.pixels->empty())
		return 1.0f;
	double totalDelta = 0.0;
	const auto pixels = first.pixels->size() / 4;
	for (size_t index = 0; index < pixels; ++index)
		totalDelta += std::abs(static_cast<int>(Luma(first.pixels->data() + index * 4)) -
							   static_cast<int>(Luma(second.pixels->data() + index * 4))) /
					  255.0;
	return static_cast<float>(totalDelta / pixels);
}
} // namespace flicker
