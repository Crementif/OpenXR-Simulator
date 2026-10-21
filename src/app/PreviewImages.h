#pragma once

#include "app/PreviewTexture.h"
#include "app/RuntimePanelRequests.h"

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
};
// Owns native preview acquisition, source selection and actual GPU/UI retirement.
// A const summary never exposes its texture or permits arbitrary native teardown.
class PreviewImages
{
  public:
	PreviewImages(UiRenderer& renderer, RuntimePanelRequests& requests);
	~PreviewImages();
	void SetScope(PanelScope scope);
	void Retained(const control::Message& message, bool preview);
	void Shared(std::uint64_t id, const RuntimeReply& reply);
	void ShareFailed(std::uint64_t id);
	void Poll();
	bool HasPendingLive() const;
	bool SelectCapture(PanelScope scope, std::uint64_t capture);
	void SelectImage(std::uint64_t id);
	void ClearSelection();
	void ReleaseSelected();
	void Follow(bool enabled) noexcept;
	bool Following() const noexcept;
	std::uint64_t SelectedId() const noexcept;
	std::optional<PreviewImageSummary> Selected() const;
	std::vector<PreviewImageSummary> Images() const;
	std::size_t ReadyCount() const;
	void BeginFrame();
	void DrawSelected(float width);
	std::vector<VkSemaphore> ReadyWaits() const;
	void Submitted(std::uint64_t serial);
	std::string TakeError();

  private:
	struct Image;
	Image* Find(std::uint64_t id) const;
	PreviewImageSummary Summary(const Image& image) const;
	UiRenderer& m_renderer;
	RuntimePanelRequests& m_requests;
	PanelScope m_scope;
	std::vector<std::unique_ptr<Image>> m_images;
	std::uint64_t m_nextImage = 1, m_selectedImage = 0, m_selectedCapture = 0;
	bool m_follow = true;
	std::string m_error;
};
} // namespace simxr::app
