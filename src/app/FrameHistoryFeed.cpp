#include "app/FrameHistoryFeed.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace simxr::app
{
void FrameHistoryFeed::SetScope(PanelScope scope)
{
	if (scope == m_scope)
		return;
	m_scope = scope;
	m_after = 0;
	if (const auto previous = m_history.Latest({scope.session, scope.generation, 0}, scope.epoch))
		m_after = m_history.At(*previous).identity.frame;
	m_newSegment = true;
	m_offset = m_history.Size() ? m_history.At(m_history.Size() - 1).seconds + .1 : 0;
}
std::string FrameHistoryFeed::Accept(const control::Message& message)
{
	using namespace panel;
	namespace tag = control::tag;
	if (!m_scope.session || U64(message, tag::Session) != m_scope.session ||
		U64(message, tag::Generation) != m_scope.generation)
		return {};
	try
	{
		Append(message);
		return {};
	}
	catch (const std::exception& error)
	{
		// Skip every frame the page named so the same page is not requested again.
		for (const auto& entry : Nested(message.Find(tag::Frames)).fields)
			m_after = std::max(m_after, U64(Nested(&entry), tag::FrameId));
		m_skipped = true;
		return error.what();
	}
}
void FrameHistoryFeed::Append(const control::Message& message)
{
	using namespace panel;
	namespace tag = control::tag;
	const auto frames = Nested(message.Find(tag::Frames));
	if (frames.fields.size() > 128)
		throw std::runtime_error("Frame history response exceeds its page limit");
	for (const auto& entry : frames.fields)
	{
		const auto frame = Nested(&entry);
		const auto id = U64(frame, tag::FrameId);
		if (id <= m_after)
			continue;
		if (U64(frame, tag::Generation) != m_scope.generation)
			throw std::runtime_error("Frame history contains another run");
		const auto prediction = Nested(frame.Find(tag::Prediction));
		const auto* time = prediction.Find(tag::Time);
		const auto* period = prediction.Find(tag::Period);
		if (!time || !time->AsI64() || !period || !period->AsI64() || *period->AsI64() <= 0)
			throw std::runtime_error("Frame history has invalid runtime timing");
		const auto timestamp = *time->AsI64();
		std::uint32_t flags = 0;
		if (m_newSegment)
		{
			const auto origin = message.Find(tag::Time);
			m_origin = origin && origin->AsI64() ? *origin->AsI64() : timestamp;
			m_previous = timestamp;
			m_offset -= double((static_cast<long double>(timestamp) - m_origin) / 1000000000.L);
			flags |= HistoryRunStart;
		}
		if ((m_after && id - m_after > 1) || timestamp < m_previous || std::exchange(m_skipped, false))
			flags |= HistoryGap;
		const auto submission = Nested(frame.Find(tag::Submission));
		if (Text(submission, tag::OutputDisposition) == "dropped" || Text(frame, tag::Stage) == "discarded")
			flags |= HistoryDropped;
		const auto elapsed = double((static_cast<long double>(timestamp) - m_origin) / 1000000000.L);
		const auto previousSeconds = m_history.Size() ? m_history.At(m_history.Size() - 1).seconds : m_offset;
		const auto interval = !m_newSegment ? (static_cast<long double>(timestamp) - m_previous) / 1000000.L
											: static_cast<long double>(*period->AsI64()) / 1000000.L;
		m_history.Append({{m_scope.session, m_scope.generation, id},
						  m_scope.epoch,
						  0,
						  std::max(previousSeconds, m_offset + elapsed),
						  float(std::max(0.L, interval)),
						  flags,
						  elapsed});
		m_previous = timestamp;
		m_after = id;
		m_newSegment = false;
	}
}
} // namespace simxr::app
