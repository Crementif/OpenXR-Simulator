#pragma once

#include "simulation/TrackingTypes.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace simxr
{
class BodyModel
{
  public:
	explicit BodyModel(const BodyProportions& proportions = {});
	void SetProportions(const BodyProportions& proportions);
	const BodyProportions& Proportions() const;
	std::uint32_t Revision() const;
	std::span<const SkeletonJoint> Skeleton() const;
	BodySample Evaluate(const PoseSample& head, const BodyControls& controls, RuntimeTime time,
						bool focused = true) const;
	static std::span<const BodyJoint> JointMap(BodyJointSet set);
	static BodyJoint TrackerJoint(TrackerRole role);
	static std::string_view TrackerRolePath(TrackerRole role);
	static std::string TrackerPersistentPath(TrackerIdentity identity);

  private:
	void Rebuild();
	BodyProportions m_proportions;
	std::uint32_t m_revision = 0;
	std::array<SkeletonJoint, BodyJointCount> m_skeleton;
};
} // namespace simxr
