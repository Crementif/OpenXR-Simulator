#include "app/mcp/McpPch.h"
#include "app/mcp/McpArtifactCalls.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpDebug.h"
#include "app/mcp/McpSequence.h"
#include "control/ControlSchema.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;
using namespace std::chrono_literals;

ArtifactCalls::ArtifactCalls(McpApplication& app, McpDebug& debug, std::string connection,
							 std::shared_ptr<McpCancellation> cancellation, double seconds, std::string operation)
	: m_app(app), m_debug(debug), m_connection(std::move(connection)), m_cancellation(std::move(cancellation)),
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
				  Disconnect();
				  return;
			  }
			  std::this_thread::sleep_for(10ms);
		  }
	  })
{}

ArtifactCalls::~ArtifactCalls()
{
	m_monitor.request_stop();
	m_monitor.join();
	if (!m_terminated && (m_cancellation->Requested() || std::chrono::steady_clock::now() >= m_deadline))
		Disconnect();
}

void ArtifactCalls::Check() const
{
	if (m_cancellation->Requested())
		throw std::runtime_error(m_operation + " was cancelled");
	if (std::chrono::steady_clock::now() >= m_deadline)
		throw std::runtime_error(m_operation + " timed out; reconnect if the debugger connection was interrupted");
}

std::shared_ptr<McpCancellation> ArtifactCalls::Begin()
{
	Check();
	auto active = std::make_shared<McpCancellation>();
	{
		std::lock_guard lock(m_mutex);
		if (m_terminated || m_cancellation->Requested() || std::chrono::steady_clock::now() >= m_deadline)
			active->Cancel();
		else
			m_active = active;
	}
	return active;
}

void ArtifactCalls::End() noexcept
{
	std::lock_guard lock(m_mutex);
	m_active.reset();
}

void ArtifactCalls::Disconnect() noexcept
{
	m_debug.Disconnect(m_connection);
}

control::Message ArtifactCalls::Request(control::OpCode opcode, std::vector<control::Field> fields,
										std::optional<std::string_view> program)
{
	const auto active = Begin();
	try
	{
		auto result = m_debug.Request(m_app, m_connection, opcode, std::move(fields), active, program);
		End();
		Check();
		const auto code = *result.Find(control::tag::Error)->AsU32();
		if (code)
		{
			const auto* detail = result.Find(control::tag::Detail);
			throw ArtifactError(detail && detail->AsText() ? std::string(*detail->AsText()) : m_operation + " failed",
								code);
		}
		return result;
	}
	catch (...)
	{
		End();
		Check();
		throw;
	}
}

Json ArtifactCalls::Sequence(control::OpCode opcode, std::vector<control::Field> fields,
							 std::optional<std::string_view> program)
{
	const auto response = Request(opcode, fields, program);
	try
	{
		return SequenceDocument(response, {opcode, std::move(fields)});
	}
	catch (...)
	{
		Disconnect();
		throw;
	}
}
}
