#pragma once

#include "platform/windows/WindowsLibrary.h"
#include <GL/gl.h>

namespace simxr::windows
{
// Each context/image owner keeps its dispatch and native dependency together.
class OpenGLLibrary final
{
  private:
	WindowsLibrary m_library{L"opengl32.dll"};

  public:
	OpenGLLibrary();
	const decltype(&::glGetError) GetError;
	const decltype(&::glGetIntegerv) GetIntegerv;
	const decltype(&::glDeleteTextures) DeleteTextures;
	const decltype(&::glFlush) Flush;
	const decltype(&::glFinish) Finish;
	const decltype(&::glEnable) Enable;
	const decltype(&::glViewport) Viewport;
	const decltype(&::glBlendFunc) BlendFunc;
	const decltype(&::glClearColor) ClearColor;
	const decltype(&::glClear) Clear;
	const decltype(&::glDrawArrays) DrawArrays;
	const decltype(&::wglGetCurrentDC) GetCurrentDC;
	const decltype(&::wglGetCurrentContext) GetCurrentContext;
	const decltype(&::wglMakeCurrent) MakeCurrent;
	const decltype(&::wglCreateContext) CreateContext;
	const decltype(&::wglDeleteContext) DeleteContext;
	const decltype(&::wglGetProcAddress) GetProcAddress;
};
} // namespace simxr::windows
