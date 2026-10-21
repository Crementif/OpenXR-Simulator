#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>

#include <sddl.h>

namespace simxr::control::windows
{
using Clock = std::chrono::steady_clock;
struct Handle
{
	HANDLE value = INVALID_HANDLE_VALUE;
	Handle() = default;
	explicit Handle(HANDLE value) : value(value) {}
	Handle(const Handle&) = delete;
	Handle& operator=(const Handle&) = delete;
	Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
	Handle& operator=(Handle&& other) noexcept
	{
		if (this != &other)
		{
			Close();
			value = std::exchange(other.value, INVALID_HANDLE_VALUE);
		}
		return *this;
	}
	~Handle()
	{
		Close();
	}
	void Close() noexcept
	{
		if (value != INVALID_HANDLE_VALUE && value)
			CloseHandle(std::exchange(value, INVALID_HANDLE_VALUE));
	}
};
inline std::runtime_error Error(const char* detail, DWORD result = GetLastError())
{
	return std::runtime_error(std::string(detail) + " (Windows error " + std::to_string(result) + ")");
}
inline Handle Event()
{
	Handle value(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	if (!value.value)
		throw Error("Create control cancellation event failed");
	return value;
}
inline std::wstring PipePath(std::string_view path)
{
	constexpr std::string_view prefix = R"(\\.\pipe\SimXR.Control.)";
	if (!path.starts_with(prefix) || path.size() <= prefix.size() || path.size() > 240)
		throw std::invalid_argument("Expected a local \\\\.\\pipe\\SimXR.Control.NAME endpoint");
	for (const unsigned char value : path.substr(prefix.size()))
		if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
			  value == '.' || value == '-' || value == '_'))
			throw std::invalid_argument("Invalid local control pipe name");
	return std::wstring(path.begin(), path.end());
}
inline std::vector<std::byte> TokenSid(HANDLE process)
{
	Handle token;
	if (!OpenProcessToken(process, TOKEN_QUERY, &token.value))
		throw Error("Query control peer token failed");
	DWORD bytes = 0;
	GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
	if (!bytes || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		throw Error("Query control peer SID size failed");
	std::vector<std::byte> storage(bytes);
	if (!GetTokenInformation(token.value, TokenUser, storage.data(), bytes, &bytes))
		throw Error("Query control peer SID failed");
	const auto sid = reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid;
	std::vector<std::byte> copy(GetLengthSid(sid));
	if (copy.empty() || !CopySid(static_cast<DWORD>(copy.size()), copy.data(), sid))
		throw Error("Copy control peer SID failed");
	return copy;
}
struct Security
{
	std::vector<std::byte> sid = TokenSid(GetCurrentProcess());
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
	Security()
	{
		LPWSTR text = nullptr;
		if (!ConvertSidToStringSidW(sid.data(), &text))
			throw Error("Format control user SID failed");
		std::wstring sddl;
		try
		{
			sddl = L"D:P(A;;GA;;;" + std::wstring(text) + L")";
		}
		catch (...)
		{
			LocalFree(text);
			throw;
		}
		LocalFree(text);
		if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
			throw Error("Create protected control pipe ACL failed");
		attributes.lpSecurityDescriptor = descriptor;
	}
	~Security()
	{
		if (descriptor)
			LocalFree(descriptor);
	}
};
inline void VerifyPeer(HANDLE pipe, bool server, const std::vector<std::byte>& expected)
{
	ULONG processId = 0;
	if (!(server ? GetNamedPipeClientProcessId(pipe, &processId) : GetNamedPipeServerProcessId(pipe, &processId)) ||
		!processId)
		throw Error("Query control pipe peer process failed");
	Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
	if (!process.value)
		throw Error("Open control pipe peer process failed");
	auto sid = TokenSid(process.value);
	if (!EqualSid(sid.data(), const_cast<std::byte*>(expected.data())))
		throw std::runtime_error("Control pipe peer belongs to another Windows user");
}
struct Timeout
{
};
struct Interrupted
{
};
inline std::atomic<unsigned> NativeChannels{0};
// The native OVERLAPPED and its backing buffer are retained across timeouts.
// One bounded native owner per connection also covers failed cancellation.
class Channel
{
	enum class Kind
	{
		None,
		Connect,
		Read,
		Write
	};
	struct Native
	{
		Handle pipe;
		Handle event = Event();
		OVERLAPPED operation{};
		std::array<std::byte, 65536> bytes;
		Kind kind = Kind::None;
		DWORD goal = 0;
		explicit Native(Handle value) : pipe(std::move(value))
		{
			operation.hEvent = event.value;
		}
	};
	std::unique_ptr<Native> m_native;
	bool m_charged = false;
	DWORD Finish(Clock::time_point deadline, HANDLE stop)
	{
		for (;;)
		{
			if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0)
				throw Interrupted{};
			const auto now = Clock::now();
			if (now >= deadline)
				throw Timeout{};
			DWORD milliseconds = 50;
			if (deadline != Clock::time_point::max())
				milliseconds = static_cast<DWORD>(
					std::min<std::int64_t>(50, std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count()));
			const HANDLE events[]{m_native->event.value, stop};
			const auto result = stop ? WaitForMultipleObjects(2, events, FALSE, milliseconds)
									 : WaitForSingleObject(events[0], milliseconds);
			if (result == WAIT_TIMEOUT)
				continue;
			if (result == WAIT_OBJECT_0 + 1)
				throw Interrupted{};
			if (result != WAIT_OBJECT_0)
				throw Error("Wait for control pipe operation failed");
			DWORD count = 0;
			if (!GetOverlappedResult(m_native->pipe.value, &m_native->operation, &count, FALSE))
			{
				const auto error = GetLastError();
				if (error == ERROR_IO_INCOMPLETE)
					continue;
				m_native->kind = Kind::None;
				throw Error("Control pipe operation failed", error);
			}
			m_native->kind = Kind::None;
			return count;
		}
	}
	void Prepare(Kind kind, DWORD goal)
	{
		if (!m_native)
			throw std::runtime_error("Control pipe is closed");
		m_native->operation = {};
		m_native->operation.hEvent = m_native->event.value;
		ResetEvent(m_native->event.value);
		m_native->goal = goal;
		m_native->kind = kind;
	}

  public:
	explicit Channel(Handle pipe)
	{
		if (NativeChannels.fetch_add(1) >= 128)
		{
			NativeChannels.fetch_sub(1);
			throw std::runtime_error("Windows control native owner capacity exhausted");
		}
		m_charged = true;
		try
		{
			m_native = std::make_unique<Native>(std::move(pipe));
		}
		catch (...)
		{
			NativeChannels.fetch_sub(1);
			m_charged = false;
			throw;
		}
	}
	~Channel()
	{
		Close();
	}
	Channel(const Channel&) = delete;
	Channel& operator=(const Channel&) = delete;
	HANDLE Value() const
	{
		return m_native ? m_native->pipe.value : INVALID_HANDLE_VALUE;
	}
	void Connect(Clock::time_point deadline, HANDLE stop)
	{
		Prepare(Kind::Connect, 0);
		if (ConnectNamedPipe(Value(), &m_native->operation))
		{
			m_native->kind = Kind::None;
			return;
		}
		const auto error = GetLastError();
		if (error == ERROR_PIPE_CONNECTED)
		{
			m_native->kind = Kind::None;
			return;
		}
		if (error != ERROR_IO_PENDING)
		{
			m_native->kind = Kind::None;
			throw Error("Accept control pipe connection failed", error);
		}
		(void)Finish(deadline, stop);
	}
	std::size_t Transfer(bool write, std::span<std::byte> bytes, Clock::time_point deadline, HANDLE stop)
	{
		if (!m_native || bytes.empty())
			throw std::runtime_error("Invalid control pipe transfer");
		const auto kind = write ? Kind::Write : Kind::Read;
		const auto goal = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), m_native->bytes.size()));
		DWORD count = 0;
		if (m_native->kind == Kind::None)
		{
			Prepare(kind, goal);
			if (write)
				std::copy_n(bytes.data(), goal, m_native->bytes.data());
			const auto complete = write ? WriteFile(Value(), m_native->bytes.data(), goal, &count, &m_native->operation)
										: ReadFile(Value(), m_native->bytes.data(), goal, &count, &m_native->operation);
			if (complete)
				m_native->kind = Kind::None;
			else if (const auto error = GetLastError(); error != ERROR_IO_PENDING)
			{
				m_native->kind = Kind::None;
				throw Error("Start control pipe transfer failed", error);
			}
		}
		else if (m_native->kind != kind || m_native->goal != goal)
			throw std::logic_error("A pending control pipe operation must be retried unchanged");
		if (m_native->kind != Kind::None)
			count = Finish(deadline, stop);
		if (!count || count > goal)
			throw std::runtime_error("Control pipe peer disconnected or returned invalid byte count");
		if (!write)
			std::copy_n(m_native->bytes.data(), count, bytes.data());
		return count;
	}
	void Close() noexcept
	{
		if (!m_native)
			return;
		if (m_native->kind != Kind::None)
		{
			CancelIoEx(Value(), &m_native->operation);
			const auto signaled = WaitForSingleObject(m_native->event.value, 1000);
			DWORD count = 0;
			const auto complete = GetOverlappedResult(Value(), &m_native->operation, &count, FALSE);
			if ((!complete && GetLastError() == ERROR_IO_INCOMPLETE) || signaled == WAIT_FAILED)
			{
				std::fputs("SimXR Windows control cancellation incomplete; preserving bounded native operation\n",
						   stderr);
				(void)m_native.release();
				m_charged = false;
				return;
			}
		}
		m_native.reset();
		if (m_charged)
			NativeChannels.fetch_sub(1);
		m_charged = false;
	}
};
} // namespace simxr::control::windows
