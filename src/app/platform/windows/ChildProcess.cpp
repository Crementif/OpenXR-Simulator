#include "app/ChildProcess.h"
#include "app/WindowsLaunch.h"

#include <cwchar>
#include <map>

namespace simxr::app
{
namespace
{
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
			environment[windows::Wide(key)] = windows::Wide(value);
		if (!profile.headsetProfile.empty())
			environment[L"SIMXR_HEADSET_PROFILE"] = windows::Wide(profile.headsetProfile);
		if (!profile.runtimeManifest.empty())
			environment[L"XR_RUNTIME_JSON"] = windows::Wide(profile.runtimeManifest);
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
} // namespace
struct ChildProcess::State
{
	windows::Handle handle;
	DWORD identity = 0;
};
ChildProcess::ChildProcess() : m_state(std::make_unique<State>()) {}
ChildProcess::~ChildProcess() = default;
std::unique_ptr<ChildProcess> ChildProcess::Launch(const LaunchProfile& profile)
{
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
