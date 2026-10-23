#pragma once

#include "control/ControlPacket.h"

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace simxr::control
{
// No connection or request was admitted; a later connection attempt is safe.
class ControlConnectTimeout : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};
class ControlExchangeTimeout : public std::runtime_error
{
  public:
	explicit ControlExchangeTimeout(RequestId request);
	RequestId Request() const noexcept;

  private:
	RequestId m_request;
};
// One synchronous exchange at a time. Timeout leaves the same exchange pending;
// Retry resumes byte/descriptor state without admitting another request.
class LocalControlClient
{
  public:
	explicit LocalControlClient(std::string path, std::chrono::milliseconds timeout = std::chrono::milliseconds{2000});
	~LocalControlClient();
	LocalControlClient(const LocalControlClient&) = delete;
	LocalControlClient& operator=(const LocalControlClient&) = delete;
	ControlPacket Request(ControlPacket request);
	ControlPacket Retry();
	std::optional<RequestId> Pending() const;
	// Interrupts an active exchange, discards its pending state, and prevents reuse.
	void Close() noexcept;

  private:
	struct State;
	std::unique_ptr<State> m_state;
};
} // namespace simxr::control
