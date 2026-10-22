#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include "control/ControlPacket.h"
#include "control/ControlSchema.h"

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
class OwnedTransfer final : public TransferHandle
{
  public:
	explicit OwnedTransfer(Handle handle) : m_handle(std::move(handle)) {}
	TransferHandleKind Kind() const noexcept override
	{
		return TransferHandleKind::OpaqueWin32;
	}
	std::intptr_t Value() const noexcept override
	{
		return reinterpret_cast<std::intptr_t>(m_handle.value);
	}
	std::shared_ptr<TransferHandle> Duplicate() const override;

  private:
	Handle m_handle;
};
inline std::shared_ptr<TransferHandle> DuplicateTransfer(HANDLE process, HANDLE handle)
{
	if (!handle || reinterpret_cast<std::intptr_t>(handle) <= 0)
		throw std::invalid_argument("Cannot transfer a null or pseudo Windows handle");
	Handle copy;
	if (!DuplicateHandle(process, handle, GetCurrentProcess(), &copy.value, 0, FALSE, DUPLICATE_SAME_ACCESS))
		throw Error("Duplicate Windows transfer handle failed");
	return std::make_shared<OwnedTransfer>(std::move(copy));
}
inline std::shared_ptr<TransferHandle> OwnedTransfer::Duplicate() const
{
	return DuplicateTransfer(GetCurrentProcess(), m_handle.value);
}
inline std::array<std::byte, 8> TransferAcknowledgement(RequestId request)
{
	std::array<std::byte, 8> bytes{};
	for (unsigned index = 0; index < bytes.size(); ++index)
		bytes[index] = static_cast<std::byte>(request.value >> (index * 8));
	return bytes;
}
inline std::size_t TransferBytes(const Message& message)
{
	const auto* field = message.Find(tag::HandleCount);
	const auto count = field ? field->AsU32() : std::optional<std::uint32_t>{0};
	if (!count || *count > MaxTransferHandles)
		throw std::invalid_argument("Invalid Windows transfer handle count");
	return *count * 8;
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
inline Handle VerifyPeer(HANDLE pipe, bool server, const std::vector<std::byte>& expected,
						 bool* nativeHandles = nullptr)
{
	ULONG processId = 0;
	if (!(server ? GetNamedPipeClientProcessId(pipe, &processId) : GetNamedPipeServerProcessId(pipe, &processId)) ||
		!processId)
		throw Error("Query control pipe peer process failed");
	Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, processId));
	const bool transferable = process.value != nullptr;
	if (!transferable)
		process = Handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
	if (!process.value)
		throw Error("Open control pipe peer process failed");
	auto sid = TokenSid(process.value);
	if (!EqualSid(sid.data(), const_cast<std::byte*>(expected.data())))
		throw std::runtime_error("Control pipe peer belongs to another Windows user");
	if (nativeHandles)
		*nativeHandles = transferable;
	return process;
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
		Handle peer;
		bool nativeHandles = false;
		std::vector<std::shared_ptr<TransferHandle>> offered;
		bool published = false;
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
	void Authenticate(bool server, const std::vector<std::byte>& sid)
	{
		m_native->peer = VerifyPeer(Value(), server, sid, &m_native->nativeHandles);
	}
	bool NativeHandles() const noexcept
	{
		return m_native && m_native->nativeHandles;
	}
	std::vector<std::byte> Offer(const ControlPacket& packet)
	{
		if (m_native->published)
			throw std::logic_error("Unacknowledged Windows handle transfer");
		m_native->offered.clear();
		const auto* field = packet.message.Find(tag::HandleCount);
		const auto count = field ? field->AsU32() : std::optional<std::uint32_t>{0};
		if (!count || *count != packet.handles.size() || *count > MaxTransferHandles || (*count && !NativeHandles()))
			throw std::invalid_argument("Windows transfer handle count mismatch");
		std::vector<std::byte> bytes;
		bytes.reserve(*count * 8);
		for (const auto& handle : packet.handles)
		{
			if (!handle || handle->Kind() != TransferHandleKind::OpaqueWin32 || handle->Value() <= 0)
				throw std::invalid_argument("Expected an owned Win32 handle");
			auto duplicate = handle->Duplicate();
			const auto value = static_cast<std::uint64_t>(duplicate->Value());
			for (unsigned shift = 0; shift < 64; shift += 8)
				bytes.push_back(static_cast<std::byte>(value >> shift));
			m_native->offered.push_back(std::move(duplicate));
		}
		return bytes;
	}
	void PublishOffer(const std::shared_ptr<PacketExposure>& exposure = {}) noexcept
	{
		// An overlapped write can reach the peer even if its CPU wait times out.
		m_native->published = !m_native->offered.empty();
		if (m_native->published && exposure)
			exposure->MarkTransferred();
	}
	void AcknowledgeOffer() noexcept
	{
		m_native->published = false;
		m_native->offered.clear();
	}
	std::vector<std::shared_ptr<TransferHandle>> Import(std::span<const std::byte> bytes)
	{
		if (!NativeHandles() || bytes.size() % 8 || bytes.size() > MaxTransferHandles * 8 || !m_native->peer.value ||
			m_native->peer.value == INVALID_HANDLE_VALUE)
			throw std::invalid_argument("Invalid Windows handle transfer");
		std::vector<std::shared_ptr<TransferHandle>> handles;
		for (std::size_t offset = 0; offset < bytes.size(); offset += 8)
		{
			std::uint64_t value = 0;
			for (unsigned index = 0; index < 8; ++index)
				value |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + index])} << (index * 8);
			if (!value || value > static_cast<std::uint64_t>(INTPTR_MAX))
				throw std::invalid_argument("Invalid Windows source handle");
			handles.push_back(DuplicateTransfer(m_native->peer.value, reinterpret_cast<HANDLE>(value)));
		}
		return handles;
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
		if (m_native->published && WaitForSingleObject(m_native->peer.value, 0) != WAIT_OBJECT_0)
		{
			// Keep sender handle numbers valid when the live peer may still be duplicating.
			// This consumes one of the 128 native channel slots until process exit.
			m_native->pipe.Close();
			std::fputs("SimXR Windows handle acknowledgement missing; preserving bounded offer\n", stderr);
			(void)m_native.release();
			m_charged = false;
			return;
		}
		m_native.reset();
		if (m_charged)
			NativeChannels.fetch_sub(1);
		m_charged = false;
	}
};
} // namespace simxr::control::windows
