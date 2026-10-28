#include "control/legacy/WindowsLegacyFiles.h"
#include "Pch.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace mcp
{
std::uint64_t WindowsLegacyProcessId()
{
#ifdef _WIN32
	return GetCurrentProcessId();
#else
	return static_cast<std::uint64_t>(getpid());
#endif
}

bool ClaimWindowsLegacyCommand(const std::filesystem::path& source, const std::filesystem::path& destination)
{
#ifdef _WIN32
	if (MoveFileW(source.c_str(), destination.c_str()))
		return true;
	const auto error = GetLastError();
	if (error == ERROR_FILE_NOT_FOUND || error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
		return false;
	throw std::system_error(static_cast<int>(error), std::system_category(), "Claim Windows compatibility command");
#else
	if (syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(), RENAME_NOREPLACE) == 0)
		return true;
	if (errno == ENOENT || errno == EEXIST)
		return false;
	throw std::system_error(errno, std::generic_category(), "Claim compatibility fixture command");
#endif
}

static std::filesystem::path CreateTemporaryOutput(const std::filesystem::path& destination)
{
#ifdef _WIN32
	static std::atomic<std::uint64_t> s_sequence{0};
	for (unsigned attempt = 0; attempt < 64; ++attempt)
	{
		auto temporary = destination;
		temporary += L".tmp." + std::to_wstring(WindowsLegacyProcessId()) + L"." +
					 std::to_wstring(s_sequence.fetch_add(1, std::memory_order_relaxed));
		const HANDLE file =
			CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			const auto error = GetLastError();
			if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
				continue;
			throw std::system_error(static_cast<int>(error), std::system_category(),
									"Create temporary compatibility output");
		}
		if (!CloseHandle(file))
		{
			const auto error = GetLastError();
			DeleteFileW(temporary.c_str());
			throw std::system_error(static_cast<int>(error), std::system_category(),
									"Close temporary compatibility output");
		}
		return temporary;
	}
	throw std::runtime_error("Too many existing temporary compatibility outputs");
#else
	auto pattern = destination.native() + ".tmp.XXXXXX";
	const int descriptor = mkstemp(pattern.data());
	if (descriptor < 0)
		throw std::system_error(errno, std::generic_category(), "Create temporary compatibility output");
	const int closed = close(descriptor);
	const int error = errno;
	try
	{
		if (closed != 0)
			throw std::system_error(error, std::generic_category(), "Close temporary compatibility output");
		return std::filesystem::path(pattern);
	}
	catch (...)
	{
		unlink(pattern.c_str());
		throw;
	}
#endif
}

static void ReplaceOutput(const std::filesystem::path& temporary, const std::filesystem::path& destination)
{
#ifdef _WIN32
	if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
								"Publish compatibility output");
#else
	std::filesystem::rename(temporary, destination);
#endif
}

WindowsLegacyOutput::WindowsLegacyOutput(std::filesystem::path destination) : m_destination(std::move(destination))
{
	try
	{
		m_temporary = CreateTemporaryOutput(m_destination);
		m_stream.exceptions(std::ios::badbit | std::ios::failbit);
		m_stream.open(m_temporary, std::ios::binary | std::ios::trunc);
	}
	catch (...)
	{
		Abort();
		throw;
	}
}

WindowsLegacyOutput::~WindowsLegacyOutput()
{
	if (!m_committed)
		Abort();
}

void WindowsLegacyOutput::Write(std::span<const std::byte> bytes)
{
	if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
		throw std::length_error("Compatibility output exceeds stream width");
	m_stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void WindowsLegacyOutput::Commit()
{
	m_stream.close();
	ReplaceOutput(m_temporary, m_destination);
	m_committed = true;
}

void WindowsLegacyOutput::Abort() noexcept
{
	try
	{
		m_stream.exceptions(std::ios::goodbit);
		if (m_stream.is_open())
			m_stream.close();
	}
	catch (...)
	{
	}
	std::error_code ignored;
	std::filesystem::remove(m_temporary, ignored);
}

std::string ReadWindowsLegacyDocument(const std::filesystem::path& path)
{
	if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)))
		throw std::invalid_argument("Compatibility command must be a regular file");
	const auto size = std::filesystem::file_size(path);
	if (size > 64 * 1024)
		throw std::invalid_argument("Compatibility command exceeds 64 KiB");
	std::ifstream input;
	input.exceptions(std::ios::badbit | std::ios::failbit);
	input.open(path, std::ios::binary);
	std::string document(static_cast<std::size_t>(size), '\0');
	input.read(document.data(), static_cast<std::streamsize>(document.size()));
	if (input.peek() != std::char_traits<char>::eof())
		throw std::runtime_error("Compatibility command changed while reading");
	input.clear();
	input.close();
	return document;
}
} // namespace mcp
