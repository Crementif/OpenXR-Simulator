#pragma once

#include "app/services/FrameIdentity.h"

#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace simxr::app
{
class PreviewImages;
class RuntimeSession;

// The CSV worker owns copied frame metadata. Retained snapshot readers stay
// protected from eviction until their sequential image exports settle.
class RangeExportTask
{
public:
	explicit RangeExportTask(RuntimeSession& session);
	bool Start(FrameIdentity first, FrameIdentity last, std::uint64_t epoch, const std::string& path);
	void Poll();
	bool Busy() const;

	const std::string& Metadata() const noexcept
	{
		return m_metadata;
	}

	const std::string& Images() const noexcept
	{
		return m_images;
	}

private:
	class Protection
	{
	public:
		Protection(PreviewImages& preview, std::span<const std::uint64_t> images);
		~Protection();
		Protection(const Protection&) = delete;
		Protection& operator=(const Protection&) = delete;

	private:
		PreviewImages& m_preview;
	};

	void Finish(std::string result);
	RuntimeSession& m_session;
	std::future<void> m_csv;
	std::vector<std::pair<std::uint64_t, std::string>> m_queue;
	std::optional<Protection> m_protection;
	std::uint64_t m_epoch = 0;
	std::size_t m_next = 0;
	bool m_requested = false;
	std::string m_path, m_metadata, m_images;
};
} // namespace simxr::app
