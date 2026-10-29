#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include "control/ControlPacket.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace simxr::control::windows
{
using Clock = std::chrono::steady_clock;

struct Handle
{
	HANDLE value = INVALID_HANDLE_VALUE;
	Handle() = default;
	explicit Handle(HANDLE value);
	Handle(const Handle&) = delete;
	Handle& operator=(const Handle&) = delete;
	Handle(Handle&& other) noexcept;
	Handle& operator=(Handle&& other) noexcept;
	~Handle();
	void Close() noexcept;
};

std::runtime_error Error(const char* detail, DWORD result = GetLastError());
std::shared_ptr<TransferHandle> DuplicateTransfer(HANDLE process, HANDLE handle);
std::array<std::byte, 8> TransferAcknowledgement(RequestId request);
std::size_t TransferBytes(const Message& message);
Handle Event();
std::wstring PipePath(std::string_view path);
std::vector<std::byte> TokenSid(HANDLE process);

struct Security
{
	std::vector<std::byte> sid;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
	Security();
	~Security();
	Security(const Security&) = delete;
	Security& operator=(const Security&) = delete;
};

Handle VerifyPeer(HANDLE pipe, bool server, const std::vector<std::byte>& expected, bool* nativeHandles = nullptr);

struct Timeout
{
};

struct Interrupted
{
};

// The native OVERLAPPED and its backing buffer are retained across timeouts.
// One bounded native owner per connection also covers failed cancellation.
class Channel
{
public:
	explicit Channel(Handle pipe);
	~Channel();
	Channel(const Channel&) = delete;
	Channel& operator=(const Channel&) = delete;
	HANDLE Value() const;
	void Authenticate(bool server, const std::vector<std::byte>& sid);
	bool NativeHandles() const noexcept;
	std::vector<std::byte> Offer(const ControlPacket& packet);
	void PublishOffer(const std::shared_ptr<PacketExposure>& exposure = {}) noexcept;
	void AcknowledgeOffer() noexcept;
	std::vector<std::shared_ptr<TransferHandle>> Import(std::span<const std::byte> bytes);
	void Connect(Clock::time_point deadline, HANDLE stop);
	std::size_t Transfer(bool write, std::span<std::byte> bytes, Clock::time_point deadline, HANDLE stop);
	void Close() noexcept;

private:
	enum class Kind;
	struct Native;
	DWORD Finish(Clock::time_point deadline, HANDLE stop);
	void Prepare(Kind kind, DWORD goal);
	std::unique_ptr<Native> m_native;
	bool m_charged = false;
};
} // namespace simxr::control::windows
