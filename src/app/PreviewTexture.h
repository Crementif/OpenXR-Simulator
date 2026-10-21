#pragma once

#include "app/VulkanDeviceAccess.h"
#include "control/ImageInspection.h"

#include <memory>
#include <span>

namespace simxr::app
{
// UI-thread owner of one private native copy and its sampled descriptors.
class PreviewTexture
{
  public:
	PreviewTexture(const VulkanDeviceAccess& device, control::ImageDescription description);
	~PreviewTexture();
	PreviewTexture(const PreviewTexture&) = delete;
	PreviewTexture& operator=(const PreviewTexture&) = delete;
	control::ImageDeviceIdentity Identity() const;
	std::shared_ptr<control::TransferHandle> ConsumerFence();
	void Import(control::ImageDescription description,
				std::span<const std::shared_ptr<control::TransferHandle>> handles);
	void AbandonUnsent();
	CompletionStatus Poll();
	bool Ready() const;
	const control::ImageDescription& Description() const;
	std::size_t SliceCount() const;
	void BeginFrame();
	void Draw(float width);
	VkSemaphore ReadyWait() const;
	void Submitted(std::uint64_t serial);
	void ObserveCompletion(std::uint64_t serial);
	bool CanRetire(std::uint64_t completed);

  private:
	struct State;
	std::unique_ptr<State> m_state;
};
} // namespace simxr::app
