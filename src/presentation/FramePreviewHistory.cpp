#include "presentation/FramePreviewHistory.h"
#include "Pch.h"
#include "graphics/FrameOutput.h"

namespace simxr
{
FramePreviewHistory::FramePreviewHistory()
	: m_worker(
		  [this]
		  {
			  Run();
		  })
{
}
FramePreviewHistory::~FramePreviewHistory()
{
	{
		std::lock_guard lock(m_mutex);
		m_stopping = true;
	}
	m_changed.notify_all();
	m_worker.join();
}
void FramePreviewHistory::Configure(std::uint64_t bytes)
{
	if (bytes < (std::uint64_t{MinMiB} << 20) || bytes > (std::uint64_t{MaxMiB} << 20) || bytes % SlotBytes)
		throw std::invalid_argument("GPU history budget must be 64–16384 MiB in multiples of 4 MiB");
	m_budget.SetCapacity(bytes / SlotBytes);
	while (m_budget.InUse() > m_budget.Capacity())
		if (!EvictRaw())
			break;
	m_changed.notify_all();
}
void FramePreviewHistory::ConfigureRam(std::uint64_t bytes)
{
	if (bytes < (std::uint64_t{MinMiB} << 20) || bytes > (std::uint64_t{MaxMiB} << 20) || bytes % SlotBytes)
		throw std::invalid_argument("RAM history budget must be 64–16384 MiB in multiples of 4 MiB");
	m_ramLimit.store(bytes);
	TrimRam();
}
std::string FramePreviewHistory::Error() const
{
	std::lock_guard lock(m_mutex);
	return m_error.empty() && m_gpuOnly ? "Vulkan H.264 encode/decode unavailable; retaining GPU history only"
										: m_error;
}
std::shared_ptr<FramePreviewHistory::Entry> FramePreviewHistory::EvictRaw()
{
	std::lock_guard lock(m_mutex);
	for (auto it = m_recent.begin(); it != m_recent.end(); ++it)
	{
		const auto& output = (*it)->preview.output;
		if (output && !output->CanArchive())
		{
			auto retired = std::move(*it);
			m_recent.erase(it);
			m_frames.erase(retired->preview.key);
			return retired;
		}
	}
	return {};
}
std::shared_ptr<FrameReservation> FramePreviewHistory::Reserve(FrameKey key)
{
	auto reservation = m_budget.Reserve();
	while (!reservation)
	{
		auto retired = EvictRaw();
		if (!retired)
			return {};
		retired.reset();
		reservation = m_budget.Reserve();
	}
	// Allocate both indexes before native submission. Append only publishes pixels.
	auto entry = std::make_shared<Entry>();
	entry->preview.key = key;
	entry->admission = reservation;
	{
		std::lock_guard lock(m_mutex);
		std::erase_if(m_recent,
					  [&](const auto& frame)
					  {
						  if (frame->preview.submitted || !frame->admission.expired())
							  return false;
						  m_frames.erase(frame->preview.key);
						  return true;
					  });
		const auto [found, inserted] = m_frames.emplace(key, entry);
		if (!inserted)
			throw std::logic_error("Duplicate preview admission");
		try
		{
			m_recent.push_back(std::move(entry));
		}
		catch (...)
		{
			m_frames.erase(found);
			throw;
		}
	}
	return reservation;
}
void FramePreviewHistory::Append(FrameKey key, std::shared_ptr<const FrameOutput> output, bool clear)
{
	{
		std::lock_guard lock(m_mutex);
		auto found = m_frames.find(key);
		if (found == m_frames.end())
		{
			if (!clear)
				throw std::logic_error("Preview publication without admission");
			found = m_frames.emplace(key, std::make_shared<Entry>()).first;
		}
		auto& entry = *found->second;
		if (entry.preview.submitted)
			throw std::logic_error("Duplicate preview publication");
		if (output && !output->CanArchive())
			m_gpuOnly = true;
		entry.preview = {key, std::move(output), true, clear};
		if (clear)
		{
			entry.counter = m_ramBytes;
			entry.charge = sizeof(Entry) + 128;
			m_ramBytes->fetch_add(entry.charge);
		}
	}
	TrimRam();
	m_changed.notify_one();
}
void FramePreviewHistory::TrimRam()
{
	{
		std::lock_guard lock(m_mutex);
		if (m_trimming)
			return;
		m_trimming = true;
	}
	for (;;)
	{
		std::shared_ptr<Entry> retired;
		FramePreview decoded;
		{
			std::lock_guard lock(m_mutex);
			if (RamBytes() <= RamLimit())
			{
				m_trimming = false;
				return;
			}
			const auto found = std::find_if(m_frames.begin(), m_frames.end(),
											[](const auto& entry)
											{
												return entry.second->charge != 0;
											});
			if (found == m_frames.end())
			{
				m_trimming = false;
				return;
			}
			retired = std::move(found->second);
			m_frames.erase(found);
			if (m_decoded.key == retired->preview.key)
				decoded = std::exchange(m_decoded, {});
		}
	}
}
std::vector<FrameKey> FramePreviewHistory::Frames(SessionGeneration generation, std::size_t limit) const
{
	std::vector<FrameKey> frames;
	std::lock_guard lock(m_mutex);
	frames.reserve(std::min(m_frames.size(), limit));
	for (auto it = m_frames.rbegin(); it != m_frames.rend() && frames.size() < limit; ++it)
	{
		const auto& entry = it->second;
		if (entry->preview.submitted && entry->preview.key.generation == generation &&
			(entry->preview.output || entry->archive || entry->preview.clear))
			frames.push_back(entry->preview.key);
	}
	std::reverse(frames.begin(), frames.end());
	return frames;
}
FramePreview FramePreviewHistory::Find(FrameKey key)
{
	std::lock_guard lock(m_mutex);
	const auto found = m_frames.find(key);
	if (found == m_frames.end() || !found->second->preview.submitted)
		return {};
	const auto& entry = found->second;
	if (entry->preview.output || entry->preview.clear)
		return entry->preview;
	if (m_decoded.key == key && m_decoded.output)
		return m_decoded;
	if (!entry->archive)
		return {};
	m_requested = key;
	m_changed.notify_one();
	auto pending = entry->preview;
	pending.pending = true;
	return pending;
}
void FramePreviewHistory::Run() noexcept
{
	bool worked = false, retry = false;
	for (;;)
	{
		std::shared_ptr<Entry> encode, decode;
		FramePreview retired;
		{
			std::unique_lock lock(m_mutex);
			if (!worked && retry)
				m_changed.wait_for(lock, std::chrono::milliseconds(5));
			worked = retry = false;
			if (m_stopping)
				return;
			if (!m_error.empty())
			{
				m_changed.wait(lock,
							   [this]
							   {
								   return m_stopping;
							   });
				return;
			}
			const auto requested = m_frames.find(m_requested);
			if (requested != m_frames.end() && requested->second->archive && m_decoded.key != m_requested)
				decode = requested->second;
			const auto recent = std::min<std::size_t>(32, m_budget.Capacity() / 2);
			if (m_recent.size() > recent)
			{
				const auto& oldest = m_recent.front();
				if (oldest->preview.submitted && oldest->preview.output && oldest->preview.output->CanArchive())
					encode = oldest;
			}
			if (!encode && !decode)
			{
				m_changed.wait(lock);
				continue;
			}
			if (decode)
				retired = std::exchange(m_decoded, {});
		}
		retired = {};
		try
		{
			if (encode)
			{
				const auto source = encode->preview.output;
				if (source && source->PollCompletion() == CompletionStatus::Pending)
				{
					encode.reset();
					retry = true;
				}
				if (encode)
				{
					if (source && source->PollCompletion() == CompletionStatus::Failed)
						throw std::runtime_error("History source GPU work failed");
					auto archive = source ? source->Archive() : nullptr;
					worked = true;
					if (source && !archive)
						throw std::runtime_error("H.264 history encoder unavailable");
					std::shared_ptr<const FrameOutput> released;
					{
						std::lock_guard lock(m_mutex);
						encode->archive = std::move(archive);
						encode->counter = m_ramBytes;
						encode->charge = sizeof(Entry) + 128 + (encode->archive ? encode->archive->Bytes() : 0);
						m_ramBytes->fetch_add(encode->charge);
						released = std::move(encode->preview.output);
						m_recent.pop_front();
					}
				}
			}
			encode.reset();
			TrimRam();
			if (decode)
			{
				auto output = decode->archive->Decode(m_budget);
				retry |= !output;
				worked |= bool(output);
				if (output)
				{
					std::lock_guard lock(m_mutex);
					if (m_requested == decode->preview.key && m_frames.contains(decode->preview.key))
						m_decoded = {decode->preview.key, std::move(output), true};
				}
			}
		}
		catch (const std::exception& error)
		{
			std::lock_guard lock(m_mutex);
			m_error = error.what();
		}
		catch (...)
		{
			std::lock_guard lock(m_mutex);
			m_error = "Unknown history worker failure";
		}
	}
}
} // namespace simxr
