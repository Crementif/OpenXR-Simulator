#include "presentation/FrameDiagnosticsJournal.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace simxr
{
FrameDiagnosticsJournal::FrameDiagnosticsJournal(std::size_t capacity, TimestampSource timestamp)
	: m_timestamp(std::move(timestamp)), m_events(ValidateCapacity(capacity))
{
}

FrameDiagnosticsJournal::~FrameDiagnosticsJournal()
{
	// Release callback captures while the journal mutex and storage still exist.
	// A last-reader destructor may inspect the journal during this release.
	auto timestamp = std::move(m_timestamp);
}

std::size_t FrameDiagnosticsJournal::ValidateCapacity(std::size_t capacity)
{
	if (!capacity || capacity > 8192)
		throw std::invalid_argument("Frame diagnostic capacity out of bounds");
	return capacity;
}

void FrameDiagnosticsJournal::CopyDetail(FrameDiagnostic& event, std::string_view detail) noexcept
{
	auto bytes = std::min(detail.size(), event.detail.size());
	if (bytes < detail.size())
	{
		// Keep a complete UTF-8 prefix when the caller supplies a longer message.
		while (bytes && (static_cast<unsigned char>(detail[bytes]) & 0xC0) == 0x80)
		{
			--bytes;
		}
	}
	if (bytes)
		std::copy_n(detail.data(), bytes, event.detail.data());
	event.detailLength = static_cast<std::uint16_t>(bytes);
	event.detailTruncated = bytes < detail.size();
}

FrameDiagnostic FrameDiagnosticsJournal::Trace(FrameTraceKind kind, FrameKey frame, RuntimeTime time, XrResult result,
											   FrameDiagnosticCategory category, std::uint64_t related,
											   std::string_view detail) noexcept
{
	FrameDiagnostic event;
	event.frame = frame;
	event.time = time;
	event.kind = kind;
	event.category = category;
	event.result = result;
	event.related = related;
	CopyDetail(event, detail);
	try
	{
		event.hostTime =
			m_timestamp ? m_timestamp()
						: std::chrono::duration_cast<Duration>(std::chrono::steady_clock::now().time_since_epoch());
		std::lock_guard lock(m_mutex);
		if (m_next == std::numeric_limits<std::uint64_t>::max())
			return event;
		event.sequence = m_next++;
		m_events[(event.sequence - 1) % m_events.size()] = event;
		m_retained = std::min(m_retained + 1, m_events.size());
	}
	catch (...)
	{
		// Diagnostic failure cannot alter API/native acceptance or infer progress.
	}
	return event;
}

FrameDiagnostics FrameDiagnosticsJournal::Read(std::uint64_t after, std::size_t count,
											   std::optional<FrameKey> frame) const
{
	if (count > 256)
		throw std::invalid_argument("Frame diagnostic page exceeds 256 events");
	std::lock_guard lock(m_mutex);
	FrameDiagnostics snapshot;
	snapshot.capacity = m_events.size();
	snapshot.next = m_next;
	snapshot.oldest = m_next - m_retained;
	snapshot.dropped = snapshot.oldest - 1;
	snapshot.events.reserve(std::min(count, m_retained));
	for (auto sequence = snapshot.oldest; sequence < snapshot.next; ++sequence)
	{
		const auto& event = m_events[(sequence - 1) % m_events.size()];
		if (sequence <= after || (frame && event.frame != *frame))
			continue;
		if (snapshot.events.size() == count)
		{
			snapshot.truncated = true;
			break;
		}
		snapshot.events.push_back(event);
	}
	return snapshot;
}
} // namespace simxr
