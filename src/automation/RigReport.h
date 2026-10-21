#pragma once

#include "automation/RigRunner.h"

namespace simxr::automation
{
std::string SerializeRigJson(const RigSnapshot& snapshot);
std::string SerializeRigJunit(const RigSnapshot& snapshot);
} // namespace simxr::automation
