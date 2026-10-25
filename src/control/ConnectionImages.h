#pragma once

#include "control/ImageInspection.h"
#include "presentation/FramePreviewHistory.h"
#include "presentation/SnapshotStore.h"

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace simxr::control
{
struct ImageSource
{
	ControlSessionId session;
	FrameKey frame;
	CaptureId capture;
	CaptureTarget target;
	std::optional<DisplayTick> imageTick;
	auto operator<=>(const ImageSource&) const = default;
};

// Private service owner. Its caller provides exclusive request execution, never
// a held connection mutex: provider calls and last-reader destruction can reenter.
class ConnectionImages
{
	enum class ResourceKind
	{
		Reader,
		Delivery,
		Export
	};

  public:
	struct RequestFailure
	{
		ErrorCode error;
		const char* detail;
	};

	// Keep detached native readers beyond the caller's single-flight execution.
	// Destruction can then start another request on this connection safely.
	class RetiredResource
	{
		friend class ConnectionImages;
		std::shared_ptr<const void> m_value;
	};

	// A new table entry is not public until both response framing and replay
	// caching succeed. Rollback detaches into retired storage before the gate
	// reopens; destruction happens afterward and never marks handles transferred.
	class Publication
	{
		friend class ConnectionImages;

	  public:
		Publication(ConnectionImages& owner, RetiredResource& retired);
		~Publication();
		Publication(const Publication&) = delete;
		Publication& operator=(const Publication&) = delete;
		void Commit() noexcept;

	  private:
		void Track(ResourceKind kind, std::uint64_t id) noexcept;
		ConnectionImages& m_owner;
		RetiredResource& m_retired;
		std::optional<ResourceKind> m_kind;
		std::uint64_t m_id = 0;
		bool m_committed = false;
	};

	ConnectionImages();
	~ConnectionImages();
	ConnectionImages(const ConnectionImages&) = delete;
	ConnectionImages& operator=(const ConnectionImages&) = delete;
	// Reuse requires source identity and the selected image tick. Display images
	// can change without a new submitted application frame.
	void Retain(Message& response, ImageSource source, std::shared_ptr<const FrameOutput> output,
				const ImageInspectionProvider& provider, bool nativeHandles, Publication& publication,
				bool includeSession = true);
	void StartVideo(Message& response, ImageSource source, std::vector<PreviewClipFrame> frames,
					Publication& publication);
	Message Dispatch(const ControlPacket& incoming, ControlPacket& outgoing, std::uint16_t minor, bool nativeHandles,
					 Publication& publication, RetiredResource& retired);

  private:
	std::shared_ptr<const void> Release(ResourceKind kind, std::uint64_t id) noexcept;
	template <class Resource> struct ImageEntry
	{
		std::uint64_t id = 0, reader = 0;
		ImageSource source;
		std::shared_ptr<Resource> value;
	};
	template <class Resource> class ImageTable
	{
	  public:
		ImageTable();
		void CheckCapacity() const;
		std::shared_ptr<const ImageEntry<Resource>> Find(std::uint64_t id) const;
		std::shared_ptr<const ImageEntry<Resource>> Find(const ImageSource& source) const;
		std::shared_ptr<const ImageEntry<Resource>> Add(std::shared_ptr<Resource> value, ImageSource source,
														std::uint64_t reader = 0);
		std::shared_ptr<const ImageEntry<Resource>> Release(std::uint64_t id) noexcept;

	  private:
		std::vector<std::shared_ptr<ImageEntry<Resource>>> m_entries;
		std::uint64_t m_next = 1;
	};
	ImageTable<ImageReader> m_images;
	ImageTable<ImageDelivery> m_deliveries;
	ImageTable<ImageExport> m_exports;
};
} // namespace simxr::control
