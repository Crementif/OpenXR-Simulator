#include "Pch.h"
#include "app/platform/ChildProcess.h"

#include <stdexcept>

namespace simxr::app
{
struct ChildProcess::State
{
};
ChildProcess::ChildProcess() : m_state(std::make_unique<State>())
{
}
ChildProcess::~ChildProcess() = default;
std::unique_ptr<ChildProcess> ChildProcess::Launch(const LaunchProfile&)
{
	throw std::runtime_error("Start the app separately, then connect to it.");
}
std::uint64_t ChildProcess::Id() const noexcept
{
	return 0;
}
std::optional<int> ChildProcess::PollExit()
{
	return {};
}
} // namespace simxr::app
