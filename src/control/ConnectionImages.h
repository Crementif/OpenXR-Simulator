#pragma once

#include "control/ImageInspection.h"
#include "presentation/SnapshotStore.h"

#include <memory>
#include <optional>

namespace simxr::control
{
struct ImageSource
{
	ControlSessionId session;
	FrameKey frame;
	CaptureId capture;
	CaptureTarget target;
	auto operator<=>(const ImageSource&) const = default;
};

// Private service owner. Its caller provides exclusive request execution, never
// a held connection mutex: provider calls and last-reader destruction can reenter.
class ConnectionImages
{
	struct State;
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
	// Deduplicates the exact immutable source before invoking the provider.
	void Retain(Message& response, ImageSource source, std::shared_ptr<const FrameOutput> output,
				const ImageInspectionProvider& provider, bool nativeHandles, Publication& publication,
				bool includeSession = true);
	Message Dispatch(const ControlPacket& incoming, ControlPacket& outgoing, std::uint16_t minor, bool nativeHandles,
					 Publication& publication, RetiredResource& retired);

  private:
	std::shared_ptr<const void> Release(ResourceKind kind, std::uint64_t id) noexcept;
	std::unique_ptr<State> m_state;
};
} // namespace simxr::control
