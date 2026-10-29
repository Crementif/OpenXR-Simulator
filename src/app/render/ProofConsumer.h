#pragma once

#include "probes/CompanionFrameReceiver.h"

#include <cstdint>
#include <map>
#include <vector>

namespace simxr::app
{
class UiRenderer;

class ProofConsumer
{
public:
	ProofConsumer(UiRenderer& renderer, int descriptor, bool verifyPixels);
	~ProofConsumer();
	ProofConsumer(const ProofConsumer&) = delete;
	ProofConsumer& operator=(const ProofConsumer&) = delete;
	void Poll();
	void Draw();
	std::vector<VkSemaphore> ReadyWaits() const;
	void Submitted(std::uint64_t serial);
	bool FinishIfReady();
	bool ReadyForScreenshot() const;

private:
	struct Texture
	{
		CompanionProofFrame frame;
		VkDescriptorSet descriptor;
		bool consumed = false;
	};

	UiRenderer& m_renderer;
	CompanionFrameReceiver m_receiver;
	std::map<std::uint32_t, Texture> m_textures;
	std::vector<std::uint32_t> m_drawn;
	std::uint64_t m_allSampledSerial = 0;
	bool m_verifyPixels;
	bool m_finished = false;
};
} // namespace simxr::app
