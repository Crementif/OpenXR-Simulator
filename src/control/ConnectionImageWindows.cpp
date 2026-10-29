#include "control/ConnectionImageWindows.h"
#include "Pch.h"
#include "control/ControlRequestError.h"

namespace simxr::control
{
std::uint64_t ConnectionImageWindows::Store(ControlSessionId session, std::shared_ptr<FrameImageWindow> window)
{
	if (m_reader.window || !m_next)
		throw ControlRequestError{ErrorCode::Capacity, "Release the existing image window before starting another"};
	m_reader = {session, std::move(window)};
	m_id = m_next++;
	return m_id;
}

ImageWindowReader ConnectionImageWindows::Read(std::uint64_t id) const
{
	if (!id || id != m_id)
		throw ControlRequestError{ErrorCode::NotFound, "Image window is absent or released"};
	return m_reader;
}

ImageWindowReader ConnectionImageWindows::Release(std::uint64_t id) noexcept
{
	if (!id || id != m_id)
		return {};
	m_id = 0;
	m_reader.window->Cancel();
	return std::exchange(m_reader, {});
}
} // namespace simxr::control
