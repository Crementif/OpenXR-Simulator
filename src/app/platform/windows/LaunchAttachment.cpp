#include "Pch.h"
#include "app/services/LaunchAttachment.h"
#include "app/RuntimeEndpoints.h"

#include <stdexcept>

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

void LaunchAttachment::Initialize(const LaunchProfile& profile)
{
	m_customEndpoint = Environment(profile, "SIMXR_CONTROL_SOCKET");
	for (const auto& endpoint : DiscoverRuntimeEndpoints(false))
		m_previous.insert(endpoint);
	if (!m_customEndpoint.empty())
	{
		(void)LocalPipePath(m_customEndpoint);
		if (RuntimeEndpointExists(m_customEndpoint))
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
