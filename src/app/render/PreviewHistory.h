#pragma once

#include "app/render/PreviewTexture.h"
#include "app/services/FrameIdentity.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace simxr::app
{
class UiRenderer;

struct PreviewShot
{
	std::size_t slot = 0;
	FrameIdentity frame;
	std::uint64_t epoch = 0;
	float aspect = 9.f / 16;
};

// A bounded GPU cache of live and requested historical thumbnails. Copies use
// the UI frame's GPU commands; nothing is read back. Slots and resources retire
// only after the UI frames that used them have completed.
class PreviewHistory
{
public:
	static constexpr std::uint64_t DefaultBudget = 32ull << 20;
	PreviewHistory(UiRenderer& renderer, std::uint64_t budget = DefaultBudget);
	~PreviewHistory();
	PreviewHistory(const PreviewHistory&) = delete;
	PreviewHistory& operator=(const PreviewHistory&) = delete;

	// Keeps one eye of each displayed live image.
	void Offer(PreviewTexture& source, std::uint32_t layer, FrameIdentity frame, std::uint64_t epoch);
	// The newest kept image at or shortly before a frame of the same run.
	std::optional<PreviewShot> Nearest(FrameIdentity frame, std::uint64_t epoch) const;

	void BeginThumbnails();
	void RequestThumbnail(FrameIdentity frame, std::uint64_t epoch);
	std::optional<PreviewShot> MissingThumbnail() const;
	void UnavailableThumbnail(FrameIdentity frame, std::uint64_t epoch);

	void BeginFrame();
	void Poll();
	void Draw(const PreviewShot& shot, float width, ImVec4 crop = {0, 0, 1, 1});
	void DrawThumbnail(const PreviewShot& shot, ImVec2 minimum, ImVec2 maximum, ImVec4 crop);
	void Record(VkCommandBuffer command);
	void Submitted(std::uint64_t serial);

	void BeginRetirement();
	bool RetirementComplete() const noexcept;
	void PreserveRetired() noexcept;

	float Aspect() const noexcept;
	std::uint64_t Bytes() const noexcept;

	const std::vector<PreviewShot>& Shots() const noexcept
	{
		return m_shots;
	}

	std::uint64_t Budget() const noexcept
	{
		return m_budget;
	}

	const std::string& Error() const noexcept
	{
		return m_error;
	}

private:
	struct Resources;

	struct Copy
	{
		VkImage source = VK_NULL_HANDLE;
		std::uint32_t layer = 0;
		VkExtent2D extent{};
		std::size_t slot = 0;
		PreviewShot previous;
	};

	bool Prepare(const control::ImageDescription& description);
	void Retire();
	void CollectRetired();
	UiRenderer& m_renderer;
	std::uint64_t m_budget;
	std::unique_ptr<Resources> m_resources, m_retired;
	std::vector<PreviewShot> m_shots, m_wanted, m_unavailable;
	std::vector<std::uint64_t> m_lastUse;
	std::vector<std::size_t> m_sampled;
	std::size_t m_next = 0;
	std::optional<Copy> m_copy;
	std::optional<control::ImageDescription> m_pendingDescription;
	bool m_recorded = false;
	std::optional<std::size_t> m_display, m_displayed;
	FrameIdentity m_lastFrame;
	std::uint64_t m_lastEpoch = 0;
	float m_aspect = 9.f / 16;
	std::string m_error;
};
} // namespace simxr::app
