#include "app/services/InspectionWorkspace.h"
#include "Pch.h"
#include "graphics/SnapshotBudget.h"

namespace simxr::app
{
namespace wire = control;
namespace tag = wire::tag;
using namespace panel;

InspectionWorkspace::InspectionWorkspace(RuntimePanelRequests& requests) : m_requests(requests)
{
	m_captures.reserve(SnapshotBudget::Capacity());
}

void InspectionWorkspace::SetScope(PanelScope scope, bool clearSelection)
{
	if (m_scope.epoch != scope.epoch)
		m_revisions.clear();
	m_scope = scope;
	if (clearSelection)
		m_selectedLocal = m_selectedId = 0;
}

void InspectionWorkspace::SetCapacity(std::size_t capacity) noexcept
{
	m_capacity = capacity;
}

InspectionCapture* InspectionWorkspace::Find(std::uint64_t id)
{
	const auto found = std::find_if(m_captures.begin(), m_captures.end(),
									[&](const auto& capture) { return capture.id == id && capture.scope == m_scope; });
	return found == m_captures.end() ? nullptr : &*found;
}

bool InspectionWorkspace::Add(std::uint64_t id, std::uint64_t frame)
{
	if (!id)
		return false;
	if (auto capture = Find(id))
	{
		// Pause can announce a reserved capture before a later Step completes it.
		if (frame)
			capture->frame = frame;
		return true;
	}
	if (m_captures.size() >= m_capacity.value_or(SnapshotBudget::Capacity()))
	{
		const auto oldest = std::find_if(m_captures.begin(), m_captures.end(), [&](const auto& capture) {
			return capture.local != m_selectedLocal && (capture.released || !Boolean(capture.metadata, tag::Pinned));
		});
		if (oldest == m_captures.end())
		{
			if (std::all_of(m_captures.begin(), m_captures.end(), [](const auto& capture) {
					return !capture.released && Boolean(capture.metadata, tag::Pinned);
				}))
				m_error = "History is full. Unpin a snapshot to free space.";
			return false;
		}
		if (oldest->inspection && oldest->scope.epoch == m_scope.epoch)
			m_requests.Release(wire::OpCode::ReleaseInspection, tag::InspectionId, oldest->inspection);
		m_captures.erase(oldest);
	}
	InspectionCapture capture;
	capture.id = id;
	capture.frame = frame;
	capture.local = m_nextLocal++;
	capture.scope = m_scope;
	m_captures.push_back(std::move(capture));
	return true;
}

void InspectionWorkspace::MarkUnavailable(std::uint64_t capture)
{
	if (auto current = Find(capture))
		current->released = true;
}

void InspectionWorkspace::MarkReleased(std::uint64_t capture)
{
	if (auto current = Find(capture))
	{
		ReleaseInspection(*current);
		current->released = true;
	}
}

bool InspectionWorkspace::Select(std::uint64_t id)
{
	const auto capture = Find(id);
	if (!capture)
		return false;
	m_selectedId = id;
	m_selectedLocal = capture->local;
	return !capture->released;
}

void InspectionWorkspace::SelectLocal(std::uint64_t local)
{
	for (const auto& capture : m_captures)
		if (capture.local == local)
		{
			m_selectedLocal = local;
			m_selectedId = capture.id;
			return;
		}
}

void InspectionWorkspace::SelectImage(PanelScope scope, std::uint64_t id)
{
	m_selectedId = id;
	m_selectedLocal = 0;
	for (const auto& capture : m_captures)
		if (capture.scope == scope && capture.id == id)
			m_selectedLocal = capture.local;
}

std::uint64_t InspectionWorkspace::SelectedId() const noexcept
{
	return m_selectedId;
}

const InspectionCapture* InspectionWorkspace::Selected() const
{
	for (const auto& capture : m_captures)
		if (capture.local == m_selectedLocal)
			return &capture;
	return nullptr;
}

std::span<const InspectionCapture> InspectionWorkspace::Captures() const noexcept
{
	return m_captures;
}

std::vector<std::uint64_t> InspectionWorkspace::CaptureIds() const
{
	std::vector<std::uint64_t> result;
	for (const auto& capture : m_captures)
		if (!capture.released && capture.scope.epoch == m_scope.epoch)
			result.push_back(capture.id);
	return result;
}

void InspectionWorkspace::ReleaseInspection(InspectionCapture& capture)
{
	if (capture.inspection && capture.scope.epoch == m_scope.epoch)
		m_requests.Release(wire::OpCode::ReleaseInspection, tag::InspectionId, capture.inspection);
	capture.inspection = 0;
	capture.document = {};
}

void InspectionWorkspace::ReleaseSelected()
{
	for (auto& capture : m_captures)
		if (capture.local == m_selectedLocal)
		{
			ReleaseInspection(capture);
			if (capture.scope != m_scope)
				capture.released = true;
		}
}

void InspectionWorkspace::InspectSelected()
{
	const auto capture = Selected();
	if (!capture || capture->released || capture->scope != m_scope ||
		!Nested(capture->metadata.Find(tag::Frame)).Find(tag::Submission) ||
		m_requests.Has(PanelWork::Inspect, capture->id))
		return;
	m_requests.Send(wire::OpCode::InspectFrame, PanelWork::Inspect,
					{wire::Field::U64(tag::Session, m_scope.session, true),
					 wire::Field::U64(tag::Generation, m_scope.generation, true),
					 wire::Field::U64(tag::CaptureId, capture->id, true)},
					capture->id);
}

bool InspectionWorkspace::Handle(const PanelTask& task, const wire::Message& message)
{
	switch (task.kind)
	{
	case PanelWork::Capture:
		if (auto capture = Find(task.id))
		{
			auto metadata = message;
			auto phase = Text(message, tag::Phase);
			capture->metadata = std::move(metadata);
			capture->phase = std::move(phase);
			capture->frame = Frame(message);
			if (capture->local == m_selectedLocal && !capture->document.Find(tag::Document))
				InspectSelected();
		}
		break;
	case PanelWork::Inspect:
	{
		const auto inspection = U64(message, tag::InspectionId);
		try
		{
			if (auto capture = Find(task.id); capture && !capture->released)
			{
				auto prepared = message;
				const auto previous = capture->inspection;
				capture->document = std::move(prepared);
				capture->inspection = inspection;
				if (previous && previous != inspection)
					m_requests.Release(wire::OpCode::ReleaseInspection, tag::InspectionId, previous);
			}
			else
				m_requests.Release(wire::OpCode::ReleaseInspection, tag::InspectionId, inspection);
		}
		catch (...)
		{
			m_requests.Release(wire::OpCode::ReleaseInspection, tag::InspectionId, inspection);
			throw;
		}
		break;
	}
	case PanelWork::Revision:
		if (m_revisions.size() < 64)
		{
			// Prepare copied bytes before publishing a cache entry. A failed
			// allocation must not leave an empty revision occupying its slot.
			auto prepared = message;
			m_revisions.insert_or_assign(task.id, std::move(prepared));
		}
		break;
	default:
		return false;
	}
	return true;
}

void InspectionWorkspace::RequestRevision(std::uint64_t id, std::uint64_t documentEpoch)
{
	if (documentEpoch == m_scope.epoch && m_revisions.size() < 64 && !m_requests.Has(PanelWork::Revision, id))
		m_requests.Send(wire::OpCode::FeatureRevision, PanelWork::Revision,
						{wire::Field::U64(tag::RevisionId, id, true)}, id);
}

const wire::Message* InspectionWorkspace::Revision(std::uint64_t id, std::uint64_t documentEpoch) const
{
	if (documentEpoch != m_scope.epoch)
		return nullptr;
	const auto found = m_revisions.find(id);
	return found == m_revisions.end() ? nullptr : &found->second;
}

std::string InspectionWorkspace::TakeError()
{
	return std::exchange(m_error, {});
}
} // namespace simxr::app
