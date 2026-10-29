#pragma once

#include "app/render/PreviewHistory.h"
#include "app/render/PreviewTexture.h"
#include "app/services/RuntimePanelRequests.h"

#include <optional>

namespace simxr::app
{
class UiRenderer;

struct PreviewImageSummary
{
	std::uint64_t id = 0, reader = 0, frame = 0, capture = 0;
	PanelScope scope;
	bool live = false, released = false, ready = false;
	control::ImageDescription description;
	std::size_t slices = 0;
	std::optional<control::TargetKind> target;
	std::uint32_t layer = 0, view = 0;
	std::optional<std::uint64_t> imageTick;
	std::shared_ptr<const control::Message> state;
};

// Owns native preview acquisition, source selection and actual GPU/UI retirement.
// A const summary never exposes its texture or permits arbitrary native teardown.
class PreviewImages
{
public:
	PreviewImages(UiRenderer& renderer, RuntimePanelRequests& requests);
	~PreviewImages();
	void SetScope(PanelScope scope);
	void SetCapacity(std::size_t capacity) noexcept;
	void BeginRetirement();
	bool RetirementComplete() const;
	void PreserveUnfinished() noexcept;
	void Retained(const control::Message& message, bool preview, bool thumbnail = false);
	void Shared(std::uint64_t id, const RuntimeReply& reply);
	void ShareFailed(std::uint64_t id);
	void Poll();
	void ProtectExports(std::span<const std::uint64_t> images);
	bool HasPendingLive() const;
	bool HasPendingThumbnail() const;
	bool SelectCapture(PanelScope scope, std::uint64_t capture);
	void SelectImage(std::uint64_t id);
	bool SelectFrame(PanelScope scope, std::uint64_t frame);
	void ClearSelection();
	void ReleaseLive();
	void ReleaseInactive();
	void ReleaseSelected();
	// Shows the newest live image instead of a chosen one.
	void ShowLive(bool enabled) noexcept;
	bool ShowingLive() const noexcept;
	std::uint64_t SelectedId() const noexcept;
	std::optional<PreviewImageSummary> Selected() const;
	std::vector<PreviewImageSummary> Images() const;
	std::size_t ReadyCount() const;
	std::uint64_t SnapshotChargeBytes() const;
	void BeginFrame();
	void DrawSelected(float width, std::optional<std::size_t> slice = {}, ImVec4 crop = {0, 0, 1, 1},
					  bool correctProportions = true);
	void DrawImage(std::uint64_t id, float width, std::optional<std::size_t> slice = {}, ImVec4 crop = {0, 0, 1, 1},
				   bool correctProportions = true);
	std::vector<VkSemaphore> ReadyWaits() const;
	void RecordTransfers(VkCommandBuffer command);
	void Submitted(std::uint64_t serial);

	PreviewHistory& History() noexcept
	{
		return m_history;
	}

	const PreviewHistory& History() const noexcept
	{
		return m_history;
	}

	std::string TakeError();

private:
	struct Image;

	struct PendingImage
	{
		control::Message message;
		bool live = false, thumbnail = false;
	};

	std::size_t Capacity() const noexcept;
	Image* Find(std::uint64_t id) const;
	PreviewImageSummary Summary(const Image& image) const;

	std::optional<PendingImage> m_pending;
	std::optional<std::size_t> m_capacity;
	UiRenderer& m_renderer;
	RuntimePanelRequests& m_requests;
	PanelScope m_scope;
	std::vector<std::unique_ptr<Image>> m_images;
	PreviewHistory m_history;
	std::uint64_t m_nextImage = 1, m_selectedImage = 0, m_selectedCapture = 0;
	bool m_live = true;
	bool m_memoryBlocked = false, m_retiring = false;
	std::uint64_t m_selectedFrame = 0;
	std::string m_error;
};
} // namespace simxr::app
