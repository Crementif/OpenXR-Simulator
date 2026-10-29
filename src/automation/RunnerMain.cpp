#include "automation/RunnerMain.h"
#include "Pch.h"

#include "automation/RigRunner.h"
#include "control/ControlSchema.h"
#include "simxr/Version.h"
#if defined(__linux__)
#include "platform/linux/InstallUse.h"
#endif
#if defined(__linux__) || defined(_WIN32)
#include "control/LocalControlClient.h"
#endif

#include <csignal>

static volatile std::sig_atomic_t s_interrupted = 0;

static void Interrupt(int)
{
	s_interrupted = 1;
}

static std::uint64_t Integer(std::string_view text)
{
	std::uint64_t value = 0;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
	if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
		throw std::invalid_argument("Expected unsigned integer");
	return value;
}

static std::filesystem::path Utf8Path(std::string_view path)
{
	return std::filesystem::path{std::u8string{path.begin(), path.end()}};
}

static std::string ReadRig(const std::string& path)
{
	std::ifstream input(Utf8Path(path), std::ios::binary);
	if (!input)
		throw std::runtime_error("Cannot open rig: " + path);
	std::string source;
	std::array<char, 65536> bytes;
	while (input)
	{
		input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		const auto count = static_cast<std::size_t>(input.gcount());
		if (count > simxr::automation::MaxRigSourceBytes - source.size())
			throw std::length_error("Rig source exceeds 64 MiB");
		source.append(bytes.data(), count);
	}
	if (input.bad() || !input.eof())
		throw std::runtime_error("Cannot read rig: " + path);
	input.clear();
	input.close();
	if (!input)
		throw std::runtime_error("Cannot close rig: " + path);
	return source;
}

static void Write(const std::string& path, const std::string& text)
{
	if (path.empty())
		return;
	std::ofstream output(Utf8Path(path), std::ios::binary | std::ios::trunc);
	if (!output)
		throw std::runtime_error("Cannot open report: " + path);
	output << text << '\n';
	output.close();
	if (!output)
		throw std::runtime_error("Cannot write report: " + path);
}

int simxr::automation::RunRunner(int argc, char** argv)
{
	try
	{
		simxr::automation::RigOptions options;
		std::string socket, script, json, junit;
		for (int i = 1; i < argc; ++i)
		{
			const std::string_view flag = argv[i];
			if (flag == "--version")
			{
				std::printf("simxr_runner %s\n", simxr::Version);
				return 0;
			}
			if (flag == "--help")
			{
				std::puts("Usage: simxr_runner --socket PATH --rig PATH [--session ID --generation ID] [--seed N] "
						  "[--deadline-ms N] [--json PATH] [--junit PATH]");
				return 0;
			}
			if (flag != "--socket" && flag != "--rig" && flag != "--session" && flag != "--generation" &&
				flag != "--seed" && flag != "--deadline-ms" && flag != "--json" && flag != "--junit")
				throw std::invalid_argument("Unknown option: " + std::string{flag});
			if (i + 1 == argc || !argv[i + 1][0] || std::string_view{argv[i + 1]}.starts_with("--"))
				throw std::invalid_argument("Missing nonempty value for " + std::string{flag});
			const std::string value = argv[++i];
			if (flag == "--socket")
				socket = value;
			else if (flag == "--rig")
				script = value;
			else if (flag == "--session" || flag == "--generation")
			{
				const auto identity = Integer(value);
				if (!identity)
					throw std::invalid_argument("Session and generation identifiers must be nonzero");
				if (flag == "--session")
					options.session.value = identity;
				else
					options.generation = identity;
			}
			else if (flag == "--seed")
				options.seed = Integer(value);
			else if (flag == "--deadline-ms")
			{
				const auto count = Integer(value);
				if (!count || count > 3600000)
					throw std::invalid_argument("Deadline must be 1 through 3600000 ms");
				options.deadline = std::chrono::milliseconds{static_cast<std::int64_t>(count)};
			}
			else if (flag == "--json")
				json = value;
			else if (flag == "--junit")
				junit = value;
		}
		if (socket.empty() || script.empty())
			throw std::invalid_argument("--socket and --rig are required");
		if (static_cast<bool>(options.session.value) != static_cast<bool>(options.generation))
			throw std::invalid_argument("--session and --generation must be supplied together");
		options.source = ReadRig(script);
		options.name = "@" + script;
		ValidateRigOptions(options);
#if defined(__linux__) || defined(_WIN32)
		namespace wire = simxr::control;
		wire::LocalControlClient client(socket, std::chrono::milliseconds{50});
		std::uint64_t nextRequest = 1;
		const auto exchange = [&](wire::OpCode opcode, std::vector<wire::Field> fields) {
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
		simxr::automation::RigRunner runner(exchange, [&] { client.Close(); });
		runner.Start(std::move(options));
		std::signal(SIGINT, Interrupt);
		std::signal(SIGTERM, Interrupt);
		for (;;)
		{
			const auto phase = runner.Phase();
			if (phase != simxr::automation::RigPhase::Running && phase != simxr::automation::RigPhase::Paused)
				break;
			if (s_interrupted)
				runner.Cancel();
			std::this_thread::sleep_for(std::chrono::milliseconds{10});
		}
		runner.Join();
		const auto report = runner.JsonReport();
		std::puts(report.c_str());
		Write(json, report);
		Write(junit, runner.JunitReport());
		return runner.Phase() == simxr::automation::RigPhase::Complete ? 0 : 1;
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

#if !defined(_WIN32)
int main(int argc, char** argv)
{
#if defined(__linux__)
	if (!simxr::InstallationReady())
	{
		std::fputs("OpenXR Simulator installation is being updated or is no longer available. Try again.\n", stderr);
		return 1;
	}
#endif
	return simxr::automation::RunRunner(argc, argv);
}
#endif
