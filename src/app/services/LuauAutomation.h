#pragma once

#include "app/services/ApplicationCatalog.h"

namespace simxr::app
{
class RigController;

std::uint64_t StartLuauRun(RigController& rigs, const ApplicationSnapshot& application, LuauRunRequest request,
						   const std::shared_ptr<AutomationOwner>& owner);
LuauRunStatus ReadLuauRun(const RigController& rigs, std::uint64_t run, const std::shared_ptr<AutomationOwner>& owner);
void ControlLuauRun(RigController& rigs, std::uint64_t run, LuauControl action,
					const std::shared_ptr<AutomationOwner>& owner);
} // namespace simxr::app
