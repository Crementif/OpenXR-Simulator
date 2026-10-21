#pragma once

#include <string>

namespace simxr::app
{
struct ApplicationOptions
{
	unsigned smokeFrames = 0;
	bool hidden = false;
	bool validation = false;
	std::string screenshot;
	int proofDescriptor = -1;
	bool proofVerifyPixels = false;
	std::string controlSocket;
	bool runtimeSmoke = false;
	std::string imageExport;
};

class Application
{
  public:
	explicit Application(ApplicationOptions options);
	int Run();

  private:
	ApplicationOptions m_options;
};
} // namespace simxr::app
