#pragma once

#include <string_view>
#include <vector>

namespace simxr::app
{
class RuntimeSession;

class ConnectionDialog
{
  public:
	explicit ConnectionDialog(std::string_view endpoint);
	void Draw(RuntimeSession& session, bool* open);

  private:
	std::vector<char> m_endpoint;
};
} // namespace simxr::app
