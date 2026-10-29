#include "app/mcp/McpPch.h"
#include "app/mcp/McpTransientDebug.h"
#include "app/mcp/McpApplication.h"
#include "control/ControlSchema.h"

namespace simxr::app::mcp
{
using namespace std::chrono_literals;

TransientDebug::TransientDebug(McpApplication& application, std::shared_ptr<McpCancellation> cancellation,
							   double seconds, std::string operation)
	: m_application(application), m_cancellation(std::move(cancellation)),
	  m_deadline(std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
														std::chrono::duration<double>(seconds))),
	  m_operation(std::move(operation)), m_monitor([this](std::stop_token stop) {
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
{}

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

McpConnection TransientDebug::Connect(const std::string& application)
{
	const auto cancellation = Begin();
	std::optional<McpConnection> result;
	try
	{
		Check();
		result = m_debug.Connect(m_application, application, cancellation);
	}
	catch (...)
	{
		End();
		Check();
		throw;
	}
	End();
	Check();
	if (!result)
		throw TransientError(m_operation + " operation was interrupted");
	return std::move(*result);
}

std::shared_ptr<const CachedImage> TransientDebug::Render(const std::string& connection,
														  const ImageRenderRequest& request)
{
	const auto cancellation = Begin();
	ImageRenderResult result;
	try
	{
		Check();
		result = m_debug.Render(m_application, connection, request, cancellation);
	}
	catch (...)
	{
		End();
		Check();
		throw;
	}
	End();
	Check();
	CheckResponse(result.info);
	return std::move(result.image);
}

ApplicationSnapshot TransientDebug::Application(const std::string& application)
{
	const auto cancellation = Begin();
	std::optional<ApplicationSnapshot> result;
	try
	{
		Check();
		result = m_application.Lookup(application, cancellation);
	}
	catch (...)
	{
		End();
		Check();
		throw;
	}
	End();
	Check();
	if (!result)
		throw TransientError(m_operation + " operation was interrupted");
	return std::move(*result);
}

control::Message TransientDebug::Request(const std::string& connection, control::OpCode opcode,
										 std::vector<control::Field> fields)
{
	const auto cancellation = Begin();
	control::Message response;
	try
	{
		Check();
		response = m_debug.Request(m_application, connection, opcode, std::move(fields), cancellation);
	}
	catch (...)
	{
		End();
		Check();
		throw;
	}
	End();
	Check();
	CheckResponse(response);
	return response;
}

void TransientDebug::CheckResponse(const control::Message& response)
{
	const auto code = *response.Find(control::tag::Error)->AsU32();
	if (code)
	{
		const auto* detail = response.Find(control::tag::Detail);
		throw TransientError(detail && detail->AsText() ? std::string(*detail->AsText()) : m_operation + " failed",
							 code);
	}
}

}
