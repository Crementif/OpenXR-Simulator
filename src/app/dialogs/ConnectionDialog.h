#pragma once

#include <string_view>
#include <functional>
#include <string>
#include <vector>

namespace simxr::app
{
class RuntimeSession;

class ConnectionDialog
{
  public:
	explicit ConnectionDialog(std::string_view endpoint);
	void Draw(RuntimeSession& session, bool* open, const std::function<void(std::string)>& connect,
			  const std::function<void()>& disconnect);

  private:
	std::vector<char> m_endpoint;
};
} // namespace simxr::app
