#include "app/mcp/McpPch.h"
#include "app/mcp/McpImageCache.h"
#include "control/ControlSchema.h"

namespace simxr::app::mcp
{
bool CacheableImageSource(std::uint32_t targetKind, bool hasTickField, bool hasValidTick) noexcept
{
	return (!hasTickField || hasValidTick) &&
		(targetKind != static_cast<std::uint32_t>(control::TargetKind::VirtualDisplay) || hasValidTick);
}

ImageCache::ImageCache(std::size_t maxBytes, std::size_t maxEntries)
	: m_maxBytes(maxBytes), m_maxEntries(maxEntries)
{
}

std::shared_ptr<const CachedImage> ImageCache::Find(const std::string& key)
{
	std::lock_guard lock(m_mutex);
	for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
	{
		if (it->key == key)
		{
			auto image = it->image;
			m_entries.splice(m_entries.begin(), m_entries, it);
			return image;
		}
	}
	return {};
}

void ImageCache::Insert(std::string key, std::shared_ptr<const CachedImage> image)
{
	if (!image || key.size() > m_maxBytes || image->data.size() > m_maxBytes - key.size() ||
		image->mediaType.size() > m_maxBytes - key.size() - image->data.size() || !m_maxEntries)
		return;
	const auto bytes = key.size() + image->data.size() + image->mediaType.size();
	std::lock_guard lock(m_mutex);
	for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
	{
		if (it->key == key)
		{
			m_entries.splice(m_entries.begin(), m_entries, it);
			return;
		}
	}
	while (!m_entries.empty() && (m_entries.size() >= m_maxEntries || m_bytes > m_maxBytes - bytes))
	{
		m_bytes -= m_entries.back().bytes;
		m_entries.pop_back();
	}
	m_entries.push_front({std::move(key), std::move(image), bytes});
	m_bytes += bytes;
}
} // namespace simxr::app::mcp
