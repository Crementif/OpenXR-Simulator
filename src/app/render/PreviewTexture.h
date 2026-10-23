#pragma once

#include "app/VulkanDeviceAccess.h"
#include "control/ImageInspection.h"

#include <imgui.h>
#include <memory>
#include <optional>
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
	// crop is the shown region (x0, y0, x1, y1) in normalized image coordinates;
	// width is the shown width and the height follows the region's aspect.
	void Draw(float width, std::optional<std::size_t> slice = {}, ImVec4 crop = {0, 0, 1, 1});
	VkSemaphore ReadyWait() const;
	void Submitted(std::uint64_t serial);
	void ObserveCompletion(std::uint64_t serial);
	bool CanRetire(std::uint64_t completed);
	// The private copy, for a GPU copy recorded into this frame's UI commands;
	// marks the texture as used by this frame like Draw does.
	VkImage UseForCopy();
	// Bytes held by textures preserved for the process lifetime.
	static std::uint64_t PreservedBytes();

  private:
	struct State;
	std::unique_ptr<State> m_state;
};
} // namespace simxr::app
