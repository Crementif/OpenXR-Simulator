#include "app/ChildProcess.h"

#include <stdexcept>

namespace simxr::app
{
struct ChildProcess::State
{
};
ChildProcess::ChildProcess() : m_state(std::make_unique<State>()) {}
ChildProcess::~ChildProcess() = default;
std::unique_ptr<ChildProcess> ChildProcess::Launch(const LaunchProfile&)
{
	throw std::runtime_error("Process launch is unavailable on this platform; launch externally and attach");
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
