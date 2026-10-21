#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

namespace simxr::app
{
class UiRenderer;
class ProofConsumer
{
  public:
	ProofConsumer(UiRenderer& renderer, int descriptor, bool verifyPixels);
	~ProofConsumer();
	void Poll();
	void Draw();
	std::vector<VkSemaphore> ReadyWaits() const;
	void Submitted(std::uint64_t serial);
	bool FinishIfReady();
	bool ReadyForScreenshot() const;

  private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace simxr::app
