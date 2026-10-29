#include "simulation/ViewConfiguration.h"
#include "Pch.h"
#include "math/Math.h"

namespace simxr
{
std::uint32_t ParseLaunchRenderExtent(std::string_view value)
{
	if (value.empty())
		throw std::invalid_argument("Invalid SIMXR_WIDTH or SIMXR_HEIGHT");
	std::int64_t extent;
	const auto parsed = std::from_chars(value.data(), value.data() + value.size(), extent);
	if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
		throw std::invalid_argument("Invalid SIMXR_WIDTH or SIMXR_HEIGHT");
	return static_cast<std::uint32_t>(std::clamp<std::int64_t>(extent, 16, 8192));
}

bool ValidViewFieldOfView(XrFovf fov) noexcept
{
	const auto halfPi = std::numbers::pi_v<float> / 2;
	return std::isfinite(fov.angleLeft) && std::isfinite(fov.angleRight) && std::isfinite(fov.angleDown) &&
		   std::isfinite(fov.angleUp) && std::abs(fov.angleLeft) < halfPi && std::abs(fov.angleRight) < halfPi &&
		   std::abs(fov.angleDown) < halfPi && std::abs(fov.angleUp) < halfPi && fov.angleLeft < fov.angleRight &&
		   fov.angleDown < fov.angleUp;
}

ViewConfiguration::ViewConfiguration(ViewLayout layout, const ViewSettings& settings)
	: m_layout(layout), m_settings(settings)
{
	if ((layout != ViewLayout::Stereo && layout != ViewLayout::StereoWithInset) || !std::isfinite(settings.ipd) ||
		settings.ipd < 0 || !settings.outerWidth || !settings.outerHeight ||
		(layout == ViewLayout::StereoWithInset &&
		 (!std::isfinite(settings.insetHalfAngle) || settings.insetHalfAngle <= 0 || !settings.insetWidth ||
		  !settings.insetHeight)))
		throw std::invalid_argument("Invalid view settings");
	for (const auto& fov : settings.outerFov)
	{
		if (!ValidViewFieldOfView(fov) ||
			(layout == ViewLayout::StereoWithInset &&
			 (2.0 * settings.insetHalfAngle > static_cast<double>(fov.angleRight) - fov.angleLeft ||
			  2.0 * settings.insetHalfAngle > static_cast<double>(fov.angleUp) - fov.angleDown)))
			throw std::invalid_argument("Invalid profile field of view");
	}
}

std::vector<ViewDescription> ViewConfiguration::Describe() const
{
	std::vector<ViewDescription> views{{0, ViewRole::Outer, m_settings.outerWidth, m_settings.outerHeight},
									   {1, ViewRole::Outer, m_settings.outerWidth, m_settings.outerHeight}};
	if (m_layout == ViewLayout::StereoWithInset)
	{
		views.push_back({0, ViewRole::Inset, m_settings.insetWidth, m_settings.insetHeight});
		views.push_back({1, ViewRole::Inset, m_settings.insetWidth, m_settings.insetHeight});
	}
	return views;
}

static std::pair<float, float> InsetRange(float center, float minimum, float maximum, double halfAngle)
{
	const auto clamped = std::clamp(static_cast<double>(center), static_cast<double>(minimum) + halfAngle,
									static_cast<double>(maximum) - halfAngle);
	auto lower = static_cast<float>(clamped - halfAngle);
	auto upper = static_cast<float>(clamped + halfAngle);
	if (lower == upper)
	{
		// Preserve a nonzero representable interval when both endpoints round to the center.
		lower = std::nextafter(lower, minimum);
		upper = std::nextafter(upper, maximum);
	}
	return {lower, upper};
}

LocatedViews ViewConfiguration::Locate(const TrackingSnapshot& snapshot, bool foveated) const
{
	LocatedViews result;
	result.time = snapshot.time;
	result.head = snapshot.head;
	result.gazeActive = foveated && m_layout == ViewLayout::StereoWithInset &&
						(snapshot.gaze.flags & TrackedPoseFlags) == TrackedPoseFlags;
	float horizontal = 0;
	float vertical = 0;
	for (auto description : Describe())
	{
		PoseSample eye;
		XrFovf fov;
		if (snapshot.input.eyeViews)
		{
			eye = snapshot.input.eyeViews->poses[description.eye];
			fov = snapshot.input.eyeViews->fovs[description.eye];
		}
		else
		{
			eye = snapshot.head;
			if (m_settings.ipd != 0 && !(eye.flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
				eye.flags &= ~(XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
			if (m_settings.ipd != 0 && !(eye.flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT))
				eye.flags &= ~XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
			eye.pose = Compose(snapshot.head.pose,
							   {{0, 0, 0, 1}, {(description.eye == 0 ? -0.5f : 0.5f) * m_settings.ipd, 0, 0}});
			eye.flags = FinitePoseFlags(eye.pose, eye.flags);
			fov = m_settings.outerFov[description.eye];
		}
		if (result.gazeActive)
		{
			const auto direction = Rotate(RelativePose(snapshot.gaze.pose, eye.pose).orientation, {0, 0, -1});
			horizontal = std::atan2(direction.x, -direction.z);
			vertical = std::atan2(direction.y, -direction.z);
		}

		if (description.role == ViewRole::Inset)
		{
			const auto halfAngle = std::min({static_cast<double>(m_settings.insetHalfAngle),
											 (static_cast<double>(fov.angleRight) - fov.angleLeft) / 2,
											 (static_cast<double>(fov.angleUp) - fov.angleDown) / 2});
			const auto [left, right] = InsetRange(horizontal, fov.angleLeft, fov.angleRight, halfAngle);
			const auto [down, up] = InsetRange(vertical, fov.angleDown, fov.angleUp, halfAngle);
			fov = {left, right, up, down};
		}
		result.views.push_back({description, eye, fov});
	}
	return result;
}

std::uint32_t ViewConfiguration::EyeCount() const
{
	return 2;
}

const ViewSettings& ViewConfiguration::Settings() const
{
	return m_settings;
}

} // namespace simxr
