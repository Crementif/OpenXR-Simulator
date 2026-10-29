#include "Pch.h"
#include "app/services/LaunchAttachment.h"

namespace simxr::app
{
static std::string Environment(const LaunchProfile& profile, const char* name)
{
	for (auto entry = profile.environment.rbegin(); entry != profile.environment.rend(); ++entry)
		if (entry->first == name)
			return entry->second;
	const auto value = std::getenv(name);
	return value ? value : "";
}

static std::filesystem::path DataDirectory(const LaunchProfile& profile)
{
	if (const auto directory = Environment(profile, "SIMXR_DATA_DIR"); !directory.empty())
		return directory;
	if (const auto directory = Environment(profile, "XDG_DATA_HOME"); !directory.empty())
		return std::filesystem::path(directory) / "OpenXR-Simulator";
	if (const auto home = Environment(profile, "HOME"); !home.empty())
		return std::filesystem::path(home) / ".local/share/OpenXR-Simulator";
	throw std::runtime_error("Launch attachment needs an explicit absolute SIMXR_DATA_DIR or HOME");
}

std::optional<std::filesystem::path> LaunchRuntimeDataDirectory(const LaunchProfile& profile)
{
	try
	{
		auto path = DataDirectory(profile);
		if (path.is_relative())
			path = std::filesystem::path(profile.workingDirectory) / path;
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
		if (!std::filesystem::path(custom).is_absolute())
			throw std::invalid_argument("The launched runtime endpoint must be absolute");
		return custom;
	}
	const auto directory = DataDirectory(profile);
	if (!directory.is_absolute())
		throw std::invalid_argument("The launched runtime data directory must be absolute");
	return (directory / ("control." + std::to_string(process) + ".sock")).string();
}

void LaunchAttachment::Initialize(const LaunchProfile& profile)
{
	m_customEndpoint = Environment(profile, "SIMXR_CONTROL_SOCKET");
	m_directory = DataDirectory(profile);
	if (!m_directory.is_absolute())
		throw std::runtime_error("The launched runtime data directory must be absolute");
	std::error_code error;
	for (std::filesystem::directory_iterator entry(m_directory, error), end; !error && entry != end;
		 entry.increment(error))
	{
		if (m_previous.size() == 4096)
			throw std::runtime_error("Too many existing launch endpoints");
		m_previous.insert(entry->path().string());
	}
	if (error && error != std::errc::no_such_file_or_directory)
		throw std::filesystem::filesystem_error("Cannot enumerate launch endpoints", m_directory, error);
	if (!m_customEndpoint.empty() &&
		(!std::filesystem::path(m_customEndpoint).is_absolute() ||
		 (std::filesystem::exists(m_customEndpoint) && m_customEndpoint != m_replacedEndpoint)))
		throw std::runtime_error("Launch attachment requires a new absolute local control endpoint");
}

std::string LaunchAttachment::ExpectedEndpoint(std::uint64_t process) const
{
	if (!m_customEndpoint.empty())
		return m_customEndpoint;
	return (m_directory / ("control." + std::to_string(process) + ".sock")).string();
}

bool LaunchAttachment::EndpointExists(const std::string& endpoint) const
{
	std::error_code error;
	const bool exists = std::filesystem::is_socket(endpoint, error);
	if (error && error != std::errc::no_such_file_or_directory)
		throw std::filesystem::filesystem_error("Cannot inspect launch endpoint", endpoint, error);
	return exists;
}
}
