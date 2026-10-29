#include "Pch.h"
#include "app/services/LaunchAttachment.h"
#include "app/services/RuntimeEndpoints.h"
#include "app/platform/windows/RuntimeEndpoints.h"
#include "app/platform/windows/LaunchSupport.h"
#include "app/services/Utf8Path.h"

namespace simxr::app
{
static std::string Environment(const LaunchProfile& profile, const char* name)
{
	const auto wideName = windows::Wide(name);
	for (auto entry = profile.environment.rbegin(); entry != profile.environment.rend(); ++entry)
	{
		const auto key = windows::Wide(entry->first);
		if (CompareStringOrdinal(key.c_str(), -1, wideName.c_str(), -1, TRUE) == CSTR_EQUAL)
			return entry->second;
	}
	return windows::Environment(name);
}

std::optional<std::filesystem::path> LaunchRuntimeDataDirectory(const LaunchProfile& profile)
{
	try
	{
		const auto explicitPath = Environment(profile, "SIMXR_DATA_DIR");
		const auto local = Environment(profile, "LOCALAPPDATA");
		auto path = explicitPath.empty()
						? (local.empty() ? std::filesystem::path(L".") : Utf8Path(local)) / L"OpenXR-Simulator"
						: Utf8Path(explicitPath);
		if (path.is_relative())
			path = Utf8Path(profile.workingDirectory) / path;
		if (!path.is_absolute())
			return {};
		return path;
	}
	catch (const std::exception&)
	{
		return {};
	}
}

std::string LaunchRuntimeEndpoint(const LaunchProfile& profile, std::uint64_t process)
{
	if (const auto custom = Environment(profile, "SIMXR_CONTROL_SOCKET"); !custom.empty())
	{
		(void)LocalPipePath(custom);
		return custom;
	}
	return std::string(R"(\\.\pipe\SimXR.Control.)") + std::to_string(process);
}

void LaunchAttachment::Initialize(const LaunchProfile& profile)
{
	m_customEndpoint = Environment(profile, "SIMXR_CONTROL_SOCKET");
	for (const auto& endpoint : DiscoverRuntimeEndpoints(false))
		m_previous.insert(endpoint);
	if (!m_customEndpoint.empty())
	{
		(void)LocalPipePath(m_customEndpoint);
		if (RuntimeEndpointExists(m_customEndpoint) && m_customEndpoint != m_replacedEndpoint)
			throw std::runtime_error("Launch attachment requires a new local control endpoint");
	}
}

std::string LaunchAttachment::ExpectedEndpoint(std::uint64_t process) const
{
	if (!m_customEndpoint.empty())
		return m_customEndpoint;
	return std::string(R"(\\.\pipe\SimXR.Control.)") + std::to_string(process);
}

bool LaunchAttachment::EndpointExists(const std::string& endpoint) const
{
	return RuntimeEndpointExists(endpoint);
}
}
