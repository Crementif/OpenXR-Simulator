#pragma once

namespace simxr::headset
{
// Opt-in parent-owned stdin pipe. Any byte or writer closure requests shutdown.
void ValidateHostControlPipe();
bool HostStopRequested();
} // namespace simxr::headset
