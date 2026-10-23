#include "platform/windows/OpenGLContextScope.h"
#include "Pch.h"

#include "platform/windows/OpenGLLibrary.h"

namespace simxr::windows
{
OpenGLContextScope::OpenGLContextScope(const OpenGLLibrary& gl, HDC device, HGLRC context) noexcept
	: m_gl(gl), m_previousDevice(gl.GetCurrentDC()), m_previousContext(gl.GetCurrentContext())
{
	if (!device || !context)
		return;
	m_current = m_previousDevice == device && m_previousContext == context;
	if (!m_current)
	{
		m_changed = true;
		m_current = m_gl.MakeCurrent(device, context) != FALSE;
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
	if (!m_gl.MakeCurrent(m_previousDevice, m_previousContext))
		return false;
	m_changed = false;
	return true;
}
} // namespace simxr::windows
