#include "platform/windows/OpenGLLibrary.h"
#include "Pch.h"

namespace simxr::windows
{
OpenGLLibrary::OpenGLLibrary()
	: GetError(std::bit_cast<decltype(&::glGetError)>(m_library.Procedure("glGetError"))),
	  GetIntegerv(std::bit_cast<decltype(&::glGetIntegerv)>(m_library.Procedure("glGetIntegerv"))),
	  DeleteTextures(std::bit_cast<decltype(&::glDeleteTextures)>(m_library.Procedure("glDeleteTextures"))),
	  Flush(std::bit_cast<decltype(&::glFlush)>(m_library.Procedure("glFlush"))),
	  Finish(std::bit_cast<decltype(&::glFinish)>(m_library.Procedure("glFinish"))),
	  Enable(std::bit_cast<decltype(&::glEnable)>(m_library.Procedure("glEnable"))),
	  Viewport(std::bit_cast<decltype(&::glViewport)>(m_library.Procedure("glViewport"))),
	  BlendFunc(std::bit_cast<decltype(&::glBlendFunc)>(m_library.Procedure("glBlendFunc"))),
	  ClearColor(std::bit_cast<decltype(&::glClearColor)>(m_library.Procedure("glClearColor"))),
	  Clear(std::bit_cast<decltype(&::glClear)>(m_library.Procedure("glClear"))),
	  DrawArrays(std::bit_cast<decltype(&::glDrawArrays)>(m_library.Procedure("glDrawArrays"))),
	  GetCurrentDC(std::bit_cast<decltype(&::wglGetCurrentDC)>(m_library.Procedure("wglGetCurrentDC"))),
	  GetCurrentContext(std::bit_cast<decltype(&::wglGetCurrentContext)>(m_library.Procedure("wglGetCurrentContext"))),
	  MakeCurrent(std::bit_cast<decltype(&::wglMakeCurrent)>(m_library.Procedure("wglMakeCurrent"))),
	  CreateContext(std::bit_cast<decltype(&::wglCreateContext)>(m_library.Procedure("wglCreateContext"))),
	  DeleteContext(std::bit_cast<decltype(&::wglDeleteContext)>(m_library.Procedure("wglDeleteContext"))),
	  GetProcAddress(std::bit_cast<decltype(&::wglGetProcAddress)>(m_library.Procedure("wglGetProcAddress")))
{}
} // namespace simxr::windows
