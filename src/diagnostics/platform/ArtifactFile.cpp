#include "diagnostics/platform/ArtifactFile.h"
#include "Pch.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace flicker
{
std::uint64_t ProcessId()
{
#ifdef _WIN32
	return GetCurrentProcessId();
#else
	return static_cast<std::uint64_t>(getpid());
#endif
}

std::filesystem::path CreateTemporaryArtifact(const std::filesystem::path& destination)
{
#ifdef _WIN32
	static std::atomic<std::uint64_t> s_sequence{0};
	for (unsigned attempt = 0; attempt < 64; ++attempt)
	{
		auto temporary = destination;
		temporary += L".tmp." + std::to_wstring(ProcessId()) + L"." +
					 std::to_wstring(s_sequence.fetch_add(1, std::memory_order_relaxed));
		const HANDLE file =
			CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			const auto error = GetLastError();
			if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
				continue;
			throw std::system_error(static_cast<int>(error), std::system_category(),
									"Create temporary diagnostic artifact");
		}
		if (!CloseHandle(file))
		{
			const auto error = GetLastError();
			DeleteFileW(temporary.c_str());
			throw std::system_error(static_cast<int>(error), std::system_category(),
									"Close temporary diagnostic artifact");
		}
		return temporary;
	}
	throw std::runtime_error("Too many existing temporary diagnostic artifacts");
#else
	auto pattern = destination.native() + ".tmp.XXXXXX";
	const int descriptor = mkstemp(pattern.data());
	if (descriptor < 0)
		throw std::system_error(errno, std::generic_category(), "Create temporary diagnostic artifact");
	const int closed = close(descriptor);
	const int error = errno;
	try
	{
		if (closed != 0)
			throw std::system_error(error, std::generic_category(), "Close temporary diagnostic artifact");
		return std::filesystem::path(pattern);
	}
	catch (...)
	{
		unlink(pattern.c_str());
		throw;
	}
#endif
}

void CommitArtifact(const std::filesystem::path& temporary, const std::filesystem::path& destination)
{
#ifdef _WIN32
	if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING))
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
								"Publish diagnostic artifact");
#else
	std::filesystem::rename(temporary, destination);
#endif
}
} // namespace flicker
