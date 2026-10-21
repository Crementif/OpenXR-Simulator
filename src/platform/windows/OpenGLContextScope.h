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
	OpenGLContextScope(HDC device, HGLRC context) noexcept
		: m_previousDevice(wglGetCurrentDC()), m_previousContext(wglGetCurrentContext())
	{
		if (!device || !context)
			return;
		m_current = m_previousDevice == device && m_previousContext == context;
		if (!m_current)
		{
			m_changed = true;
			m_current = wglMakeCurrent(device, context) != FALSE;
			if (!m_current)
				Restore();
		}
	}
	~OpenGLContextScope()
	{
		Restore();
	}
	OpenGLContextScope(const OpenGLContextScope&) = delete;
	OpenGLContextScope& operator=(const OpenGLContextScope&) = delete;

	bool Current() const noexcept
	{
		return m_current;
	}
	bool Restore() noexcept
	{
		if (!m_changed)
			return true;
		if (!wglMakeCurrent(m_previousDevice, m_previousContext))
			return false;
		m_changed = false;
		return true;
	}

  private:
	const HDC m_previousDevice;
	const HGLRC m_previousContext;
	bool m_current = false;
	bool m_changed = false;
};
} // namespace simxr::windows
