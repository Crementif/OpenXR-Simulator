#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace simxr::control
{
class ControlService;
class LocalControlServer
{
  public:
	LocalControlServer(ControlService& service, std::string path, std::size_t maxClients = 16);
	~LocalControlServer();
	LocalControlServer(const LocalControlServer&) = delete;
	LocalControlServer& operator=(const LocalControlServer&) = delete;
	const std::string& Path() const;
	void Close() noexcept;

  private:
	struct State;
	std::unique_ptr<State> m_state;
};
} // namespace simxr::control
