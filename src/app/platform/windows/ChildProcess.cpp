#include "app/platform/ChildProcess.h"
#include "Pch.h"
#include "app/platform/windows/LaunchSupport.h"
#include "app/platform/windows/PrivateProtonRuntime.h"

#include <cwchar>
#include <filesystem>
#include <map>

namespace simxr::app
{
static void RequireRuntimeOverride()
{
	HANDLE handle = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &handle))
		throw windows::Error("Cannot inspect runtime selection permissions");
	const windows::Handle token(handle);
	alignas(TOKEN_MANDATORY_LABEL) std::array<std::byte, sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE>
		buffer{};
	DWORD size = 0;
	if (!GetTokenInformation(token.Get(), TokenIntegrityLevel, buffer.data(), static_cast<DWORD>(buffer.size()), &size))
		throw windows::Error("Cannot inspect runtime selection integrity");
	const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
	const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
	if (!count)
		throw std::runtime_error("Cannot determine runtime selection integrity");
	if (*GetSidSubAuthority(label->Label.Sid, count - 1) > SECURITY_MANDATORY_MEDIUM_RID)
		throw std::runtime_error(
			"The OpenXR loader ignores runtime overrides in elevated processes. "
			"Run the companion without elevation, or select a provisioned private Proton prefix in the "
			"launch profile. This launch has been cancelled.");
}
struct EnvironmentOrder
{
	bool operator()(const std::wstring& left, const std::wstring& right) const
	{
		return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
									static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
	}
};
class ChildEnvironment
{
  public:
	explicit ChildEnvironment(const LaunchProfile& profile)
	{
		std::map<std::wstring, std::wstring, EnvironmentOrder> environment;
		const std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)> inherited(GetEnvironmentStringsW(),
																					 FreeEnvironmentStringsW);
		if (!inherited)
			throw windows::Error("Cannot read child environment");
		for (auto entry = inherited.get(); *entry; entry += std::wcslen(entry) + 1)
		{
			const std::wstring value(entry);
			// Preserve hidden =C: drive working-directory entries.
			const auto equals = value.find(L'=', value.front() == L'=' ? 1 : 0);
			if (equals != std::wstring::npos)
				environment[value.substr(0, equals)] = value.substr(equals + 1);
		}
		for (const auto& [key, value] : profile.environment)
		{
			const auto name = windows::Wide(key);
			if (!profile.privateProtonPrefix.empty() &&
				(CompareStringOrdinal(name.c_str(), -1, L"WINEPREFIX", -1, TRUE) == CSTR_EQUAL ||
				 CompareStringOrdinal(name.c_str(), -1, L"STEAM_COMPAT_DATA_PATH", -1, TRUE) == CSTR_EQUAL))
				throw std::invalid_argument(
					"Private Proton launch cannot override WINEPREFIX or STEAM_COMPAT_DATA_PATH");
			environment[name] = windows::Wide(value);
		}
		if (!profile.headsetProfile.empty())
			environment[L"SIMXR_HEADSET_PROFILE"] = windows::Wide(profile.headsetProfile);
		if (!profile.privateProtonPrefix.empty())
			environment.erase(L"XR_RUNTIME_JSON");
		else
		{
			if (!profile.runtimeManifest.empty())
				environment[L"XR_RUNTIME_JSON"] = windows::Wide(profile.runtimeManifest);
			const auto runtime = environment.find(L"XR_RUNTIME_JSON");
			if (runtime != environment.end() && !runtime->second.empty())
				RequireRuntimeOverride();
		}
		for (const auto& [key, value] : environment)
		{
			m_block.insert(m_block.end(), key.begin(), key.end());
			m_block.push_back(L'=');
			m_block.insert(m_block.end(), value.begin(), value.end());
			m_block.push_back(L'\0');
		}
		m_block.push_back(L'\0');
		if (environment.empty())
			m_block.push_back(L'\0');
	}
	wchar_t* Data() noexcept
	{
		return m_block.data();
	}

  private:
	std::vector<wchar_t> m_block;
};
struct ChildProcess::State
{
	windows::Handle handle;
	DWORD identity = 0;
};
ChildProcess::ChildProcess() : m_state(std::make_unique<State>()) {}
ChildProcess::~ChildProcess() = default;
std::unique_ptr<ChildProcess> ChildProcess::Launch(const LaunchProfile& profile)
{
	ValidateLaunchProfile(profile);
	ChildEnvironment environment(profile);
	const auto executable = windows::Wide(profile.executable);
	if (executable.find(L'"') != std::wstring::npos)
		throw std::invalid_argument("Executable path contains a quote");
	auto command = L"\"" + executable + L"\"";
	for (const auto& argument : profile.arguments)
		command += L" " + windows::Quote(windows::Wide(argument));
	if (command.size() >= 32767)
		throw std::invalid_argument("Windows command line exceeds 32767 characters");
	const auto directory = windows::Wide(profile.workingDirectory);
	auto child = std::unique_ptr<ChildProcess>(new ChildProcess);
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	if (!profile.privateProtonPrefix.empty())
	{
		const std::filesystem::path application(executable), workingDirectory(directory);
		if (!application.is_absolute() || !std::filesystem::is_regular_file(application))
			throw std::invalid_argument("Private Proton launch requires an absolute executable file path");
		DWORD binary = 0;
		if (!GetBinaryTypeW(application.c_str(), &binary) || binary != SCS_64BIT_BINARY)
			throw std::invalid_argument("Private Proton launch requires a readable 64-bit Windows executable");
		if (!directory.empty() && (!workingDirectory.is_absolute() || !std::filesystem::is_directory(workingDirectory)))
			throw std::invalid_argument("Private Proton launch requires an absolute working directory that exists");
		windows::RegisterPrivateProtonRuntime(profile.privateProtonPrefix, profile.runtimeManifest);
	}
	if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
						environment.Data(), directory.empty() ? nullptr : directory.c_str(), &startup, &process))
		throw windows::Error("Launch failed");
	child->m_state->handle = windows::Handle(process.hProcess);
	child->m_state->identity = process.dwProcessId;
	const windows::Handle primaryThread(process.hThread);
	return child;
}
std::uint64_t ChildProcess::Id() const noexcept
{
	return m_state->identity;
}
std::optional<int> ChildProcess::PollExit()
{
	const auto result = WaitForSingleObject(m_state->handle.Get(), 0);
	if (result == WAIT_TIMEOUT)
		return {};
	if (result == WAIT_FAILED)
		throw windows::Error("Cannot observe child exit");
	DWORD code = 0;
	if (!GetExitCodeProcess(m_state->handle.Get(), &code))
		throw windows::Error("Cannot read child exit code");
	return static_cast<int>(code);
}
} // namespace simxr::app
