#pragma once

#include "app/PreviewTexture.h"
#include "app/ShellState.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace simxr::app
{
class UiRenderer;
struct PreviewShot
{
	std::size_t slot = 0;
	FrameIdentity frame;
	std::uint64_t epoch = 0;
};
// A VRAM ring of downscaled copies of the displayed live preview, so the recent
// past can be seen while the app runs. Copies are GPU blits recorded into the
// UI frame's own commands; nothing is read back. A slot is reused, and the ring
// destroyed, only after the UI frames that used it have completed.
class PreviewHistory
{
  public:
	static constexpr std::uint64_t DefaultBudget = 512ull << 20;
	PreviewHistory(UiRenderer& renderer, std::uint64_t budget = DefaultBudget);
	~PreviewHistory();
	PreviewHistory(const PreviewHistory&) = delete;
	PreviewHistory& operator=(const PreviewHistory&) = delete;
	// Keeps one eye of a displayed live image, at most ten times a second.
	void Offer(PreviewTexture& source, std::uint32_t layer, FrameIdentity frame, std::uint64_t epoch);
	// The newest kept image at or shortly before a frame of the same run.
	std::optional<PreviewShot> Nearest(FrameIdentity frame, std::uint64_t epoch) const;
	const std::vector<PreviewShot>& Shots() const noexcept
	{
		return m_shots;
	}
	float Aspect() const noexcept;
	void Draw(const PreviewShot& shot, float width);
	void BeginFrame();
	void Record(VkCommandBuffer command);
	void Submitted(std::uint64_t serial);
	std::uint64_t Bytes() const noexcept;
	std::uint64_t Budget() const noexcept
	{
		return m_budget;
	}
	const std::string& Error() const noexcept
	{
		return m_error;
	}

	struct Resources;

  private:
	struct Copy
	{
		VkImage source = VK_NULL_HANDLE;
		std::uint32_t layer = 0;
		VkExtent2D extent{};
		std::size_t slot = 0;
	};
	bool Prepare(const control::ImageDescription& description);
	void Retire();
	UiRenderer& m_renderer;
	std::uint64_t m_budget;
	std::unique_ptr<Resources> m_resources;
	std::vector<PreviewShot> m_shots;
	std::vector<std::uint64_t> m_lastUse;
	std::size_t m_next = 0;
	std::optional<Copy> m_copy;
	std::optional<std::size_t> m_display, m_displayed;
	FrameIdentity m_lastFrame;
	std::chrono::steady_clock::time_point m_lastCapture{};
	std::string m_error;
};
} // namespace simxr::app
