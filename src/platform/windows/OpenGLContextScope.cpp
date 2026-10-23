#include "platform/windows/OpenGLContextScope.h"
#include "Pch.h"

namespace simxr::windows
{
OpenGLContextScope::OpenGLContextScope(HDC device, HGLRC context) noexcept
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

OpenGLContextScope::~OpenGLContextScope()
{
	Restore();
}

bool OpenGLContextScope::Restore() noexcept
{
	if (!m_changed)
		return true;
	if (!wglMakeCurrent(m_previousDevice, m_previousContext))
		return false;
	m_changed = false;
	return true;
}
} // namespace simxr::windows
