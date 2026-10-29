#pragma once

#include "app/services/RuntimePanelRequests.h"

#include <map>
#include <optional>
#include <span>

namespace simxr::app
{
struct InspectionCapture
{
	std::uint64_t id = 0, frame = 0, local = 0, inspection = 0;
	PanelScope scope;
	std::string phase = "pending";
	control::Message metadata, document;
	bool released = false;
};

// Owns historical CPU readers and revision documents. Remote capture identity is
// always joined with its original epoch/session/generation, never a latest view.
class InspectionWorkspace
{
public:
	explicit InspectionWorkspace(RuntimePanelRequests& requests);
	void SetScope(PanelScope scope, bool clearSelection = true);
	void SetCapacity(std::size_t capacity) noexcept;
	bool Add(std::uint64_t capture, std::uint64_t frame);
	// Remote eviction stops new reads but preserves already-retained CPU data.
	void MarkUnavailable(std::uint64_t capture);
	void MarkReleased(std::uint64_t capture);
	bool Select(std::uint64_t capture);
	void SelectLocal(std::uint64_t local);
	void SelectImage(PanelScope scope, std::uint64_t capture);
	std::uint64_t SelectedId() const noexcept;
	const InspectionCapture* Selected() const;
	std::span<const InspectionCapture> Captures() const noexcept;
	std::vector<std::uint64_t> CaptureIds() const;
	// Current capture release waits for the debugger acknowledgement; an
	// historical selection retires locally without targeting another session.
	void ReleaseSelected();
	void InspectSelected();
	bool Handle(const PanelTask& task, const control::Message& message);
	void RequestRevision(std::uint64_t id, std::uint64_t documentEpoch);
	const control::Message* Revision(std::uint64_t id, std::uint64_t documentEpoch) const;
	std::string TakeError();

private:
	InspectionCapture* Find(std::uint64_t capture);
	void ReleaseInspection(InspectionCapture& capture);
	RuntimePanelRequests& m_requests;
	PanelScope m_scope;
	std::optional<std::size_t> m_capacity;
	std::vector<InspectionCapture> m_captures;
	std::map<std::uint64_t, control::Message> m_revisions;
	std::uint64_t m_nextLocal = 1, m_selectedLocal = 0, m_selectedId = 0;
	std::string m_error;
};
} // namespace simxr::app
