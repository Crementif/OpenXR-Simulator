#pragma once

#include "graphics/FrameGpuTiming.h"
#include "graphics/FramePool.h"
#include "graphics/RotationalSource.h"
#include "graphics/SessionComposition.h"
#include "presentation/SnapshotStore.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace simxr
{
class Session;

struct FrameCaptureRequest
{
	CaptureTarget target;
	std::optional<CompositionPlan> composition;
	std::optional<LayerImageDescription> image;
	std::optional<LayerDepthDescription> depth;
	std::optional<std::array<XrRect2Di, 2>> quadBounds;
	std::shared_ptr<FrameReservation> reservation;
	bool history = false;
	bool preview = false;
	std::optional<FramePool> historyPool;
	std::shared_ptr<FrameReservation> historyReservation;
	bool IgnoreFailure(XrResult result) const noexcept;
};

struct PreparedFrameSubmission
{
	std::shared_ptr<const FrameOutput> producer;
	std::shared_ptr<const FrameOutput> output;
	std::vector<CaptureOutput> captures;
	ReprojectionInput reprojection;
	std::shared_ptr<const FrameGpuTiming> timing;
	std::shared_ptr<const FrameOutput> preview;
};

// A synchronous submission scope. Prepare records native work without executing
// it; Submit accepts it on the application queue and commits independent delivery.
// The adapter retains all native owners, including partially submitted failures.
class FrameSubmissionBackend
{
public:
	virtual ~FrameSubmissionBackend() = default;
	virtual FramePool LiveReservations() const = 0;
	virtual FramePool HistoryReservations() const = 0;
	virtual XrResult Prepare(FrameKey key, const SessionComposition& composition,
							 std::shared_ptr<FrameReservation> reservation,
							 std::span<const FrameCaptureRequest> captures, PreparedFrameSubmission& output) = 0;
	virtual XrResult Submit() = 0;
	virtual bool Submitted() const noexcept = 0;
};

class FrameSubmission final
{
public:
	FrameSubmission(Session& session, FrameSubmissionBackend& backend);
	~FrameSubmission();
	FrameSubmission(const FrameSubmission&) = delete;
	FrameSubmission& operator=(const FrameSubmission&) = delete;

	XrResult Execute(const XrFrameEndInfo& info, const SwapchainDescriptionResolver& describe);
	const CompositionPlan& Composition() const noexcept;

private:
	XrResult PrepareCaptures();
	void PrepareQuadHistory();
	void PreparePreviewHistory();

	Session& m_session;
	FrameSubmissionBackend& m_backend;
	FrameKey m_key{};
	SessionComposition m_composition;
	std::shared_ptr<FrameReservation> m_reservation;
	std::vector<FrameCaptureRequest> m_captures;
	PreparedFrameSubmission m_prepared;
	bool m_executed = false;
};
} // namespace simxr
