#include "platform/windows/WindowsRuntimeFiles.h"
#include "Pch.h"
#include "control/legacy/WindowsLegacyFiles.h"

namespace simxr::windows
{
static constexpr std::size_t s_maxLogs = 64;

WindowsRuntimeFiles::WindowsRuntimeFiles(std::filesystem::path directory) : m_directory(std::move(directory))
{
	std::filesystem::create_directories(m_directory);
	m_logs.reserve(s_maxLogs);
	m_worker = std::thread([this] { Run(); });
}
WindowsRuntimeFiles::~WindowsRuntimeFiles()
{
	Close();
}
bool WindowsRuntimeFiles::PublishStatus(std::string document)
{
	if (document.size() > 1024 * 1024)
		return false;
	std::optional<std::string> pending(std::move(document));
	{
		std::lock_guard lock(m_mutex);
		if (m_stopping)
			return false;
		m_document.swap(pending);
	}
	m_changed.notify_all();
	return true;
}
bool WindowsRuntimeFiles::Log(std::string message)
{
	if (message.size() > 4096)
		message.resize(4096);
	{
		std::lock_guard lock(m_mutex);
		if (m_stopping)
			return false;
		if (m_logs.size() == s_maxLogs)
		{
			++m_status.droppedLogs;
			return false;
		}
		m_logs.push_back(std::move(message));
	}
	m_changed.notify_all();
	return true;
}
WindowsFileStatus WindowsRuntimeFiles::Status() const
{
	std::lock_guard lock(m_mutex);
	return m_status;
}
void WindowsRuntimeFiles::Close()
{
	std::lock_guard lifecycle(m_closeMutex);
	{
		std::lock_guard lock(m_mutex);
		m_stopping = true;
	}
	m_changed.notify_all();
	if (m_worker.joinable())
		m_worker.join();
}
void WindowsRuntimeFiles::RecordError(std::string_view error) noexcept
{
	try
	{
		std::lock_guard lock(m_mutex);
		++m_status.errors;
		m_status.detail.assign(error.substr(0, 2048));
	}
	catch (...)
	{
	}
}
void WindowsRuntimeFiles::Run() noexcept
{
	try
	{
		std::vector<std::string> logs;
		logs.reserve(s_maxLogs);
		for (;;)
		{
			std::optional<std::string> document;
			bool stopping;
			{
				std::unique_lock lock(m_mutex);
				m_changed.wait(lock, [this] { return m_stopping || m_document || !m_logs.empty(); });
				document.swap(m_document);
				logs.swap(m_logs);
				stopping = m_stopping;
			}
			if (document)
				try
				{
					mcp::WindowsLegacyOutput output(m_directory / "runtime_status.json");
					output.Write(std::as_bytes(std::span(*document)));
					output.Commit();
				}
				catch (const std::exception& error)
				{
					RecordError(error.what());
				}
			if (!logs.empty())
				try
				{
					std::ofstream file(m_directory /
						("openxr_simulator." + std::to_string(mcp::WindowsLegacyProcessId()) + ".log"), std::ios::app);
					file.exceptions(std::ios::badbit | std::ios::failbit);
					for (const auto& line : logs)
						file << line << '\n';
					file.close();
				}
				catch (const std::exception& error)
				{
					RecordError(error.what());
				}
			logs.clear();
			if (stopping)
				break;
		}
	}
	catch (const std::exception& error)
	{
		RecordError(error.what());
	}
	catch (...)
	{
		RecordError("Unknown runtime file worker failure");
	}
	std::lock_guard lock(m_mutex);
	m_stopping = true;
	m_status.closed = true;
	m_changed.notify_all();
}
} // namespace simxr::windows
