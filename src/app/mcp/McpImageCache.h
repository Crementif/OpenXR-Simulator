#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>

namespace simxr::app::mcp
{
// Virtual-display pixels may change independently of their submitted frame.
bool CacheableImageSource(std::uint32_t targetKind, bool hasTickField, bool hasValidTick) noexcept;

struct CachedImage
{
	std::string data;
	std::string mediaType;
	std::size_t encodedBytes = 0;
	bool lossy = false;
};

// Cache only immutable, already encoded content. Callers must validate the
// current connection and export before looking up an identity.
class ImageCache
{
public:
	ImageCache(std::size_t maxBytes, std::size_t maxEntries);
	std::shared_ptr<const CachedImage> Find(const std::string& key);
	void Insert(std::string key, std::shared_ptr<const CachedImage> image);

private:
	struct Entry
	{
		std::string key;
		std::shared_ptr<const CachedImage> image;
		std::size_t bytes = 0;
	};

	std::mutex m_mutex;
	std::list<Entry> m_entries;
	std::size_t m_bytes = 0;
	const std::size_t m_maxBytes;
	const std::size_t m_maxEntries;
};
} // namespace simxr::app::mcp
