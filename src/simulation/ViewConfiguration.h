#pragma once

#include "simulation/TrackingTypes.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace simxr
{
// Launch resolution overrides use the same bounded dimensions on every platform.
std::uint32_t ParseLaunchRenderExtent(std::string_view value);

enum class ViewLayout
{
	Stereo,
	StereoWithInset
};

enum class ViewRole
{
	Outer,
	Inset
};

struct ViewSettings
{
	float ipd = 0.064f;
	std::uint32_t outerWidth = 1440;
	std::uint32_t outerHeight = 1600;
	std::uint32_t insetWidth = 720;
	std::uint32_t insetHeight = 800;
	float insetHalfAngle = 0.35f;
	std::array<XrFovf, 2> outerFov{{{-0.9f, 0.9f, 0.9f, -0.9f}, {-0.9f, 0.9f, 0.9f, -0.9f}}};
};

struct ViewDescription
{
	std::uint32_t eye;
	ViewRole role;
	std::uint32_t width;
	std::uint32_t height;
};

struct ViewSample
{
	ViewDescription description;
	PoseSample pose;
	XrFovf fov;
};

struct LocatedViews
{
	RuntimeTime time;
	bool gazeActive = false;
	std::vector<ViewSample> views;
	PoseSample head;
};

struct ViewQuerySnapshot
{
	RuntimeTime displayTime;
	RuntimeTime sampleTime;
	bool foveatedRequested = false;
	bool gazeActive = false;
	std::vector<XrFovf> fieldsOfView;
};

bool ValidViewFieldOfView(XrFovf fov) noexcept;

class ViewConfiguration
{
public:
	explicit ViewConfiguration(ViewLayout layout = ViewLayout::Stereo, const ViewSettings& settings = {});
	std::vector<ViewDescription> Describe() const;
	LocatedViews Locate(const TrackingSnapshot& snapshot, bool foveated = false) const;
	std::uint32_t EyeCount() const;
	const ViewSettings& Settings() const;

private:
	ViewLayout m_layout;
	ViewSettings m_settings;
};
} // namespace simxr
