#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace simxr::windows
{
// Borrowing never transfers ownership of either WGL handle. A failed switch
// can also unset the caller's context, so it needs the same restoration path.
class OpenGLContextScope final
{
  public:
	OpenGLContextScope(HDC device, HGLRC context) noexcept;
	~OpenGLContextScope();
	OpenGLContextScope(const OpenGLContextScope&) = delete;
	OpenGLContextScope& operator=(const OpenGLContextScope&) = delete;

	bool Current() const noexcept
	{
		return m_current;
	}
	bool Restore() noexcept;

  private:
	const HDC m_previousDevice;
	const HGLRC m_previousContext;
	bool m_current = false;
	bool m_changed = false;
};
} // namespace simxr::windows
