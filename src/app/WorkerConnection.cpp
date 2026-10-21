#include "app/WorkerConnection.h"
#include "control/ControlSchema.h"
#if defined(__linux__) || defined(_WIN32)
#include "control/LocalControlClient.h"
#endif

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace simxr::app
{
struct WorkerConnection::State
{
	std::string endpoint, name;
	std::uint64_t required;
	std::atomic<bool> stopping{false};
	bool failed = false;
#if defined(__linux__) || defined(_WIN32)
	std::unique_ptr<control::LocalControlClient> client;
	control::Message Request(control::OpCode code, std::vector<control::Field> fields)
	{
		control::ControlPacket packet;
		packet.message.opcode = static_cast<std::uint16_t>(code);
		packet.message.fields = std::move(fields);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		bool retry = false;
		for (;;)
		{
			if (stopping || std::chrono::steady_clock::now() >= deadline)
			{
				failed = true;
				client->Close();
				throw std::runtime_error("Worker exchange stopped or timed out; remote admission is unknown");
			}
			try
			{
				return (retry ? client->Retry() : client->Request(std::move(packet))).message;
			}
			catch (const control::ControlExchangeTimeout&)
			{
				retry = true;
			}
			catch (...)
			{
				failed = true;
				client->Close();
				throw;
			}
		}
	}
#endif
};
WorkerConnection::WorkerConnection(std::string endpoint, std::string name, std::uint64_t required)
	: m_state(std::make_unique<State>())
{
	m_state->endpoint = std::move(endpoint);
	m_state->name = std::move(name);
	m_state->required = required;
}
WorkerConnection::~WorkerConnection() = default;
void WorkerConnection::Stop() noexcept
{
	m_state->stopping = true;
}
bool WorkerConnection::Failed() const noexcept
{
	return m_state->failed || m_state->stopping;
}
control::Message WorkerConnection::Exchange(control::OpCode code, std::vector<control::Field> fields)
{
#if defined(__linux__) || defined(_WIN32)
	if (m_state->stopping || m_state->failed)
		throw std::runtime_error("Worker connection is closing");
	if (!m_state->client)
	{
		m_state->client =
			std::make_unique<control::LocalControlClient>(m_state->endpoint, std::chrono::milliseconds{50});
		auto hello = m_state->Request(control::OpCode::Hello,
									  {control::Field::Text(control::tag::ClientName, m_state->name),
									   control::Field::U64(control::tag::RequiredCapabilities, m_state->required)});
		const auto error = hello.Find(control::tag::Error);
		if (!error || error->AsU32() != 0)
		{
			m_state->failed = true;
			m_state->client->Close();
			throw std::runtime_error("Runtime did not negotiate the required worker capabilities");
		}
	}
	return m_state->Request(code, std::move(fields));
#else
	(void)code;
	(void)fields;
	throw std::runtime_error("Worker transport is unavailable on this platform");
#endif
}
} // namespace simxr::app
