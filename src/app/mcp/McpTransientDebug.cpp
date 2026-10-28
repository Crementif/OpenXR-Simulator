#include "Pch.h"
#include "app/mcp/McpTransientDebug.h"
#include "app/mcp/McpApplication.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;
using namespace std::chrono_literals;

TransientDebug::TransientDebug(McpApplication& application, std::shared_ptr<McpCancellation> cancellation,
	double seconds, std::string operation)
	: m_application(application), m_cancellation(std::move(cancellation)),
	  m_deadline(std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds))),
	  m_operation(std::move(operation)), m_monitor([this](std::stop_token stop)
	  {
		  while (!stop.stop_requested())
		  {
			  if (m_cancellation->Requested() || std::chrono::steady_clock::now() >= m_deadline)
			  {
				  std::shared_ptr<McpCancellation> active;
				  {
					  std::lock_guard lock(m_mutex);
					  m_terminated = true;
					  active = m_active;
				  }
				  if (active)
					  active->Cancel();
				  m_debug.Close();
				  return;
			  }
			  std::this_thread::sleep_for(10ms);
		  }
	  })
{
}
TransientDebug::~TransientDebug()
{
	m_monitor.request_stop();
	m_monitor.join();
}
void TransientDebug::Check()
{
	if (m_cancellation->Requested())
		throw TransientError(m_operation + " was cancelled");
	if (std::chrono::steady_clock::now() >= m_deadline)
		throw TransientError(m_operation + " timed out");
}
void TransientDebug::Wait()
{
	Check();
	std::this_thread::sleep_for(10ms);
	Check();
}
std::shared_ptr<McpCancellation> TransientDebug::Begin()
{
	Check();
	auto cancellation = std::make_shared<McpCancellation>();
	{
		std::lock_guard lock(m_mutex);
		if (m_terminated || m_cancellation->Requested() || std::chrono::steady_clock::now() >= m_deadline)
			cancellation->Cancel();
		else
			m_active = cancellation;
	}
	return cancellation;
}
void TransientDebug::End() noexcept
{
	std::lock_guard lock(m_mutex);
	m_active.reset();
}
Json TransientDebug::CheckResult(std::optional<Json> result)
{
	Check();
	if (!result)
		throw TransientError(m_operation + " operation was interrupted");
	if (result->value("isError", true))
	{
		const auto value = Json::parse(result->at("content").at(0).at("text").get<std::string>());
		throw TransientError(value.value("detail", value.value("error", m_operation + " operation failed")), value.value("code", 0u));
	}
	return std::move(*result);
}
Json TransientDebug::Call(const std::string& name, Json arguments)
{
	const auto cancellation = Begin();
	std::optional<Json> result;
	try
	{
		Check();
		result = m_debug.Call(m_application, name, arguments, cancellation);
	}
	catch (...)
	{
		End();
		throw;
	}
	End();
	return CheckResult(std::move(result));
}
Json TransientDebug::Data(const std::string& name, Json arguments)
{
	const auto result = Call(name, std::move(arguments));
	return Json::parse(result.at("content").at(0).at("text").get<std::string>());
}
Json TransientDebug::Application(const std::string& name, Json arguments)
{
	const auto cancellation = Begin();
	std::optional<Json> result;
	try
	{
		Check();
		result = m_application.Call(name, std::move(arguments), cancellation);
	}
	catch (...)
	{
		End();
		throw;
	}
	End();
	const auto checked = CheckResult(std::move(result));
	return Json::parse(checked.at("content").at(0).at("text").get<std::string>());
}
std::vector<std::byte> TransientDebug::Read(const std::string& connection, std::uint64_t ticket, std::uint64_t offset, std::uint64_t length)
{
	const auto cancellation = Begin();
	std::vector<std::byte> bytes;
	try
	{
		Check();
		bytes = m_debug.ReadArtifact(m_application, connection, ticket, offset, length, cancellation);
	}
	catch (...)
	{
		End();
		throw;
	}
	End();
	Check();
	return bytes;
}
}
