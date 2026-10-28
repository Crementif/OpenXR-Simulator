#include "app/services/WorkerConnection.h"
#include "Pch.h"
#include "control/ControlSchema.h"
#if defined(__linux__) || defined(_WIN32)
#include "control/LocalControlClient.h"
#endif


namespace simxr::app
{
#if defined(__linux__) || defined(_WIN32)
control::Message WorkerConnection::Request(control::OpCode code, std::vector<control::Field> fields)
{
	control::ControlPacket packet;
	packet.message.opcode = static_cast<std::uint16_t>(code);
	packet.message.fields = std::move(fields);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	bool retry = false;
	for (;;)
	{
		if (m_stopping || std::chrono::steady_clock::now() >= deadline)
		{
			m_failed = true;
			m_client->Close();
			throw std::runtime_error("The request stopped or timed out. It may still be running.");
		}
		try
		{
			return (retry ? m_client->Retry() : m_client->Request(std::move(packet))).message;
		}
		catch (const control::ControlExchangeTimeout&)
		{
			retry = true;
		}
		catch (...)
		{
			m_failed = true;
			m_client->Close();
			throw;
		}
	}
}
#endif
WorkerConnection::WorkerConnection(std::string endpoint, std::string name, std::uint64_t required,
								   std::optional<std::uint64_t> expectedProcess)
	: m_endpoint(std::move(endpoint)), m_name(std::move(name)), m_required(required), m_expectedProcess(expectedProcess)
{
}
WorkerConnection::~WorkerConnection() = default;
void WorkerConnection::Stop() noexcept
{
	m_stopping = true;
}
bool WorkerConnection::Failed() const noexcept
{
	return m_failed || m_stopping;
}
control::Message WorkerConnection::Exchange(control::OpCode code, std::vector<control::Field> fields)
{
#if defined(__linux__) || defined(_WIN32)
	if (m_stopping || m_failed)
		throw std::runtime_error("Worker connection is closing");
	if (!m_client)
	{
		m_client = std::make_unique<control::LocalControlClient>(m_endpoint, std::chrono::milliseconds{50});
		if (m_expectedProcess && m_client->PeerProcess() != *m_expectedProcess)
		{
			m_failed = true;
			m_client->Close();
			throw std::runtime_error("Runtime endpoint belongs to a different process");
		}
		auto hello =
			Request(control::OpCode::Hello, {control::Field::Text(control::tag::ClientName, m_name),
											 control::Field::U64(control::tag::RequiredCapabilities, m_required)});
		const auto error = hello.Find(control::tag::Error);
		if (!error || error->AsU32() != 0)
		{
			m_failed = true;
			m_client->Close();
			throw std::runtime_error("Runtime did not negotiate the required worker capabilities");
		}
		const auto capabilities = hello.Find(control::tag::Capabilities);
		m_capabilities = capabilities ? capabilities->AsU64().value_or(0) : 0;
	}
	return Request(code, std::move(fields));
#else
	(void)code;
	(void)fields;
	throw std::runtime_error("Worker transport is unavailable on this platform");
#endif
}
} // namespace simxr::app
