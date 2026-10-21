#include "automation/RigRunner.h"
#include "control/ControlSchema.h"
#if defined(__linux__) || defined(_WIN32)
#include "control/LocalControlClient.h"
#endif

#include <charconv>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace
{
volatile std::sig_atomic_t interrupted = 0;
void Interrupt(int)
{
	interrupted = 1;
}
std::uint64_t Integer(std::string_view text)
{
	std::uint64_t value = 0;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
	if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
		throw std::invalid_argument("Expected unsigned integer");
	return value;
}
void Write(const std::string& path, const std::string& text)
{
	if (path.empty())
		return;
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output << text << '\n';
	output.close();
	if (!output)
		throw std::runtime_error("Cannot write report: " + path);
}
} // namespace
int main(int argc, char** argv)
{
	try
	{
		simxr::automation::RigOptions options;
		std::string socket, script, json, junit;
		for (int i = 1; i < argc; ++i)
		{
			const std::string_view flag = argv[i];
			if (flag == "--help")
			{
				std::puts("Usage: simxr_runner --socket PATH --rig PATH [--session ID --generation ID] [--seed N] "
						  "[--deadline-ms N] [--json PATH] [--junit PATH]");
				return 0;
			}
			if (i + 1 == argc)
				throw std::invalid_argument("Missing option value");
			const std::string value = argv[++i];
			if (flag == "--socket")
				socket = value;
			else if (flag == "--rig")
				script = value;
			else if (flag == "--session")
				options.session.value = Integer(value);
			else if (flag == "--generation")
				options.generation = Integer(value);
			else if (flag == "--seed")
				options.seed = Integer(value);
			else if (flag == "--deadline-ms")
			{
				const auto count = Integer(value);
				if (!count || count > 3600000)
					throw std::invalid_argument("Deadline must be 1 through 3600000 ms");
				options.deadline = std::chrono::milliseconds{count};
			}
			else if (flag == "--json")
				json = value;
			else if (flag == "--junit")
				junit = value;
			else
				throw std::invalid_argument("Unknown option: " + std::string{flag});
		}
		if (socket.empty() || script.empty())
			throw std::invalid_argument("--socket and --rig are required");
		if (std::filesystem::file_size(script) > simxr::control::MaxPayload)
			throw std::length_error("Rig source exceeds 1 MiB");
		std::ifstream input(script, std::ios::binary);
		if (!input)
			throw std::runtime_error("Cannot open rig");
		options.source.assign(std::istreambuf_iterator<char>{input}, {});
		options.name = "@" + script;
#if defined(__linux__) || defined(_WIN32)
		namespace wire = simxr::control;
		wire::LocalControlClient client(socket, std::chrono::milliseconds{50});
		std::uint64_t nextRequest = 1;
		const auto exchange = [&](wire::OpCode opcode, std::vector<wire::Field> fields)
		{
			wire::ControlPacket request;
			request.message.opcode = static_cast<std::uint16_t>(opcode);
			request.message.requestId = wire::RequestId{nextRequest++};
			request.message.fields = std::move(fields);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
			bool retry = false;
			for (;;)
			{
				try
				{
					return (retry ? client.Retry() : client.Request(std::move(request))).message;
				}
				catch (const wire::ControlExchangeTimeout&)
				{
					retry = true;
					if (std::chrono::steady_clock::now() >= deadline)
					{
						client.Close();
						throw;
					}
				}
			}
		};
		const auto hello = exchange(
			wire::OpCode::Hello, {wire::Field::Text(wire::tag::ClientName, "simxr_runner"),
								  wire::Field::U64(wire::tag::RequiredCapabilities,
												   static_cast<std::uint64_t>(wire::Capability::SimulationCommands))});
		const auto* helloError = hello.Find(wire::tag::Error);
		if (!helloError || helloError->AsU32() != 0)
			throw std::runtime_error("Runtime does not support simulator commands");
		if (!options.session.value)
		{
			const auto sessions = exchange(wire::OpCode::Sessions, {});
			const auto* list = sessions.Find(wire::tag::Sessions);
			const auto decoded = list ? wire::DecodeFields(list->value) : wire::FieldsResult{};
			if (decoded.error != wire::DecodeError::None || decoded.fields.size() != 1)
				throw std::runtime_error("Choose --session and --generation when discovery is ambiguous");
			wire::Message session;
			session.fields = wire::DecodeFields(decoded.fields.front().value).fields;
			const auto* id = session.Find(wire::tag::Session);
			const auto* generation = session.Find(wire::tag::Generation);
			if (!id || !id->AsU64() || !generation || !generation->AsU64())
				throw std::runtime_error("Invalid session discovery result");
			options.session = wire::ControlSessionId{*id->AsU64()};
			options.generation = *generation->AsU64();
		}
		simxr::automation::RigRunner runner(exchange);
		runner.Start(std::move(options));
		std::signal(SIGINT, Interrupt);
		std::signal(SIGTERM, Interrupt);
		for (;;)
		{
			const auto state = runner.Snapshot();
			if (state.phase != simxr::automation::RigPhase::Running &&
				state.phase != simxr::automation::RigPhase::Paused)
				break;
			if (interrupted)
				runner.Cancel();
			std::this_thread::sleep_for(std::chrono::milliseconds{10});
		}
		runner.Join();
		const auto report = runner.JsonReport();
		std::puts(report.c_str());
		Write(json, report);
		Write(junit, runner.JunitReport());
		return runner.Snapshot().phase == simxr::automation::RigPhase::Complete ? 0 : 1;
#else
		throw std::runtime_error("Local runner transport is not implemented on this platform");
#endif
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "Runner: %s\n", error.what());
		return 1;
	}
}
