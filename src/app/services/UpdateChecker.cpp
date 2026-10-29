#include "app/UiPch.h"
#include "app/services/UpdateChecker.h"

#include <nlohmann/json.hpp>

namespace simxr::app
{
bool ValidReleaseRepository(std::string_view repository)
{
	const auto slash = repository.find('/');
	if (slash == 0 || slash == std::string_view::npos || slash + 1 == repository.size() || repository.size() > 150 ||
		repository.find('/', slash + 1) != std::string_view::npos)
		return false;
	const auto valid = [](char character, bool owner) {
		return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
			   (character >= '0' && character <= '9') || character == '-' ||
			   (!owner && (character == '_' || character == '.'));
	};
	return std::ranges::all_of(repository.substr(0, slash), [&](char c) { return valid(c, true); }) &&
		   std::ranges::all_of(repository.substr(slash + 1), [&](char c) { return valid(c, false); }) &&
		   repository.substr(slash + 1) != "." && repository.substr(slash + 1) != "..";
}

static std::array<std::uint32_t, 3> ReleaseVersion(std::string_view text)
{
	if (text.starts_with('v'))
		text.remove_prefix(1);
	std::array<std::uint32_t, 3> version{};
	for (std::size_t index = 0; index < version.size(); ++index)
	{
		const auto end = text.find('.');
		const auto part = text.substr(0, end);
		if (part.empty() || (part.size() > 1 && part.front() == '0'))
			throw std::runtime_error("Release version is not a stable numeric version");
		const auto parsed = std::from_chars(part.data(), part.data() + part.size(), version[index]);
		if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() ||
			(index + 1 < version.size() ? end == std::string_view::npos : end != std::string_view::npos))
			throw std::runtime_error("Release version is not a stable numeric version");
		if (end != std::string_view::npos)
			text.remove_prefix(end + 1);
	}
	return version;
}

UpdateResult ParseRelease(std::string_view repository, std::string_view current, std::string_view response)
{
	if (!ValidReleaseRepository(repository))
		throw std::invalid_argument("A public GitHub release repository is not configured");
	if (response.size() > 1024 * 1024)
		throw std::runtime_error("Release response is too large");
	const auto json = nlohmann::json::parse(response);
	if (!json.is_object() || !json.contains("tag_name") || !json["tag_name"].is_string() || !json.contains("draft") ||
		!json["draft"].is_boolean() || json["draft"].get<bool>() || !json.contains("prerelease") ||
		!json["prerelease"].is_boolean() || json["prerelease"].get<bool>())
		throw std::runtime_error("GitHub did not return a published stable release");
	const auto tag = json["tag_name"].get<std::string>();
	const auto latest = ReleaseVersion(tag);
	const auto installed = ReleaseVersion(current);
	if (latest <= installed)
		return {UpdateStatus::Current};
	return {UpdateStatus::Available, tag, "https://github.com/" + std::string(repository) + "/releases/tag/" + tag};
}

UpdateChecker::UpdateChecker(std::string repository, std::string version, Fetch fetch)
	: m_repository(std::move(repository)), m_version(std::move(version)), m_fetch(std::move(fetch))
{}

UpdateChecker::~UpdateChecker()
{
	if (m_worker.joinable())
		m_worker.join();
}

void UpdateChecker::Start()
{
	if (m_state.status == UpdateStatus::Checking)
		return;
	if (!ValidReleaseRepository(m_repository))
	{
		m_state = {UpdateStatus::Unavailable, {}, {}, "No public release repository is configured for this build."};
		return;
	}
	if (m_worker.joinable())
		m_worker.join();
	m_state = {UpdateStatus::Checking};
	m_done.store(false, std::memory_order_relaxed);
	try
	{
		m_worker = std::thread([this] {
			UpdateResult result;
			try
			{
				result = ParseRelease(m_repository, m_version, m_fetch(m_repository));
			}
			catch (const std::exception& error)
			{
				result = {UpdateStatus::Unavailable, {}, {}, error.what()};
			}
			catch (...)
			{
				result = {UpdateStatus::Unavailable, {}, {}, "Unknown update check failure"};
			}
			{
				std::lock_guard lock(m_mutex);
				m_completed = std::move(result);
			}
			m_done.store(true, std::memory_order_release);
		});
	}
	catch (const std::exception& error)
	{
		m_state = {UpdateStatus::Unavailable, {}, {}, error.what()};
	}
}

bool UpdateChecker::Poll()
{
	if (!m_done.load(std::memory_order_acquire))
		return false;
	if (m_worker.joinable())
		m_worker.join();
	std::lock_guard lock(m_mutex);
	m_state = std::move(*m_completed);
	m_completed.reset();
	m_done.store(false, std::memory_order_relaxed);
	return true;
}

const UpdateResult& UpdateChecker::State() const noexcept
{
	return m_state;
}
} // namespace simxr::app
