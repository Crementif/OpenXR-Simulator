#include "app/mcp/McpPch.h"
#include "app/mcp/McpInstance.h"
#include "app/mcp/McpCredentials.h"
#include "app/platform/FileStorage.h"
#include "app/platform/windows/LaunchSupport.h"
#include "app/platform/windows/PrivateSecurity.h"
#include "app/services/Utf8Path.h"

#include <cwchar>

namespace simxr::app
{
using Json = nlohmann::json;

static std::wstring NormalizedPath(const std::filesystem::path& path)
{
	const auto source = path.lexically_normal().native();
	if (source.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		throw std::invalid_argument("MCP app path is too long");
	const auto count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, source.data(),
									 static_cast<int>(source.size()), nullptr, 0, nullptr, nullptr, 0);
	if (!count)
		throw windows::Error("Cannot normalize MCP app path");
	std::wstring result(count, L'\0');
	if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, source.data(), static_cast<int>(source.size()),
					   result.data(), count, nullptr, nullptr, 0))
		throw windows::Error("Cannot normalize MCP app path");
	return result;
}

static std::string InstanceKey(const std::filesystem::path& executable)
{
	const auto text = windows::Utf8(NormalizedPath(executable));
	std::uint64_t first = 14695981039346656037ull, second = 7809847782465536322ull;
	for (const auto byte : text)
	{
		first = (first ^ static_cast<unsigned char>(byte)) * 1099511628211ull;
		second = (second ^ static_cast<unsigned char>(byte)) * 1099511628211ull;
	}
	static constexpr char hex[] = "0123456789abcdef";
	std::string key;
	for (const auto value : {first, second})
		for (int shift = 60; shift >= 0; shift -= 4)
			key.push_back(hex[(value >> shift) & 15]);
	return key;
}

static std::string SessionInstanceKey(const std::filesystem::path& executable)
{
	DWORD session = 0;
	if (!ProcessIdToSessionId(GetCurrentProcessId(), &session))
		throw windows::Error("Cannot identify the MCP Windows session");
	return InstanceKey(executable) + "-" + std::to_string(session);
}

static std::wstring MutexName(const std::string& key, const std::filesystem::path& directory,
							  const windows::PrivateSecurity& security)
{
	return L"Local\\OpenXRSimulator.Mcp." + security.SidText() + L"." + windows::Wide(InstanceKey(directory)) + L"." +
		   windows::Wide(key);
}

static std::filesystem::path PrivateDirectory()
{
	const auto custom = windows::Environment("SIMXR_MCP_DISCOVERY_DIR");
	const auto local = windows::Environment("LOCALAPPDATA");
	const auto directory = !custom.empty() ? Utf8Path(custom) : Utf8Path(local) / "OpenXR-Simulator-MCP";
	if (!directory.is_absolute())
		throw std::runtime_error("MCP discovery directory must be absolute");
	windows::PrivateSecurity security(true);
	if (!CreateDirectoryW(directory.c_str(), security.Attributes()) && GetLastError() != ERROR_ALREADY_EXISTS)
		throw windows::Error("Cannot create MCP discovery directory");
	windows::Handle handle(CreateFileW(directory.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
									   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
									   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	if (handle.Get() == INVALID_HANDLE_VALUE)
		throw windows::Error("Cannot open MCP discovery directory");
	BY_HANDLE_FILE_INFORMATION info{};
	if (!GetFileInformationByHandle(handle.Get(), &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
		(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
		throw std::runtime_error("MCP discovery directory cannot be a reparse point");
	security.Check(handle.Get(), SE_FILE_OBJECT, true);
	return directory;
}

static void CheckPrivateFile(const std::filesystem::path& path)
{
	windows::Handle handle(CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
									   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
									   FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	if (handle.Get() == INVALID_HANDLE_VALUE)
		throw windows::Error("Cannot inspect MCP discovery record");
	BY_HANDLE_FILE_INFORMATION info{};
	if (!GetFileInformationByHandle(handle.Get(), &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
		(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		throw std::runtime_error("MCP discovery record is not a regular private file");
	windows::PrivateSecurity security(false);
	security.Check(handle.Get(), SE_FILE_OBJECT, false);
}

class McpCreatedFile
{
public:
	explicit McpCreatedFile(HANDLE handle) : m_handle(handle) {}

	~McpCreatedFile()
	{
		if (m_handle != INVALID_HANDLE_VALUE)
			CloseHandle(m_handle);
	}

	McpCreatedFile(const McpCreatedFile&) = delete;
	McpCreatedFile& operator=(const McpCreatedFile&) = delete;

	void Close()
	{
		const auto handle = std::exchange(m_handle, INVALID_HANDLE_VALUE);
		if (!CloseHandle(handle))
			throw windows::Error("Cannot close private MCP record");
	}

private:
	HANDLE m_handle;
};

static std::filesystem::path CreatePrivateMcpFile(const std::filesystem::path& destination)
{
	windows::PrivateSecurity security(false);
	for (unsigned attempt = 0; attempt < 16; ++attempt)
	{
		auto temporary = destination;
		temporary += windows::Wide("." + NewMcpIdentifier() + ".tmp");
		const auto handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, security.Attributes(), CREATE_NEW,
										FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
		{
			if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS)
				continue;
			throw windows::Error("Cannot create private MCP record");
		}
		McpCreatedFile file(handle);
		try
		{
			file.Close();
		}
		catch (...)
		{
			DeleteFileW(temporary.c_str());
			throw;
		}
		return temporary;
	}
	throw std::runtime_error("Cannot allocate a unique MCP discovery record");
}

struct McpFileIdentity
{
	std::string volume, index;
};

static std::optional<McpFileIdentity> FileIdentity(const std::filesystem::path& path)
{
	windows::Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
									 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
									 FILE_ATTRIBUTE_NORMAL, nullptr));
	if (file.Get() == INVALID_HANDLE_VALUE)
		return {};
	BY_HANDLE_FILE_INFORMATION info{};
	if (!GetFileInformationByHandle(file.Get(), &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		return {};
	const auto index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
	return McpFileIdentity{std::to_string(info.dwVolumeSerialNumber), std::to_string(index)};
}

std::filesystem::path McpOwnExecutable()
{
	std::vector<wchar_t> buffer(512);
	while (true)
	{
		const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (!count)
			throw windows::Error("Cannot resolve the running executable");
		if (count < buffer.size())
			return std::filesystem::weakly_canonical(std::filesystem::path(std::wstring(buffer.data(), count)));
		if (buffer.size() >= 32768)
			throw std::runtime_error("Running executable path is too long");
		buffer.resize(buffer.size() * 2);
	}
}

McpInstanceDiscovery::McpInstanceDiscovery(std::filesystem::path executable, bool windowless)
	: m_executable(std::filesystem::weakly_canonical(std::move(executable))), m_directory(PrivateDirectory()),
	  m_key(SessionInstanceKey(m_executable)), m_windowless(windowless)
{
	if (!m_executable.is_absolute())
		throw std::invalid_argument("MCP app path must be absolute");
}

McpInstanceDiscovery::~McpInstanceDiscovery()
{
	if (m_child)
		CloseHandle(m_child);
}

struct McpEnvironmentOrder
{
	bool operator()(const std::wstring& left, const std::wstring& right) const
	{
		return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
									static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
	}
};

static std::vector<wchar_t> ChildEnvironment(const std::string& token)
{
	std::map<std::wstring, std::wstring, McpEnvironmentOrder> environment;
	const std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)> inherited(GetEnvironmentStringsW(),
																				 FreeEnvironmentStringsW);
	if (!inherited)
		throw windows::Error("Cannot read MCP app environment");
	for (auto entry = inherited.get(); *entry; entry += std::wcslen(entry) + 1)
	{
		const std::wstring value(entry);
		const auto equals = value.find(L'=', value.front() == L'=' ? 1 : 0);
		if (equals != std::wstring::npos)
			environment[value.substr(0, equals)] = value.substr(equals + 1);
	}
	environment[L"SIMXR_MCP_TOKEN"] = windows::Wide(token);
	std::vector<wchar_t> block;
	for (const auto& [name, value] : environment)
	{
		block.insert(block.end(), name.begin(), name.end());
		block.push_back(L'=');
		block.insert(block.end(), value.begin(), value.end());
		block.push_back(L'\0');
	}
	block.push_back(L'\0');
	return block;
}

class McpProcessAttributes
{
public:
	explicit McpProcessAttributes(std::span<HANDLE> handles)
	{
		SIZE_T size = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
		if (!size || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
			throw windows::Error("Cannot size MCP app process attributes");
		m_storage.resize(size);
		m_list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(m_storage.data());
		if (!InitializeProcThreadAttributeList(m_list, 1, 0, &size))
			throw windows::Error("Cannot prepare MCP app process attributes");
		if (!UpdateProcThreadAttribute(m_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
									   handles.size_bytes(), nullptr, nullptr))
		{
			DeleteProcThreadAttributeList(m_list);
			m_list = nullptr;
			throw windows::Error("Cannot restrict MCP app inherited handles");
		}
	}

	~McpProcessAttributes()
	{
		if (m_list)
			DeleteProcThreadAttributeList(m_list);
	}

	PPROC_THREAD_ATTRIBUTE_LIST Get() const
	{
		return m_list;
	}

private:
	std::vector<std::byte> m_storage;
	PPROC_THREAD_ATTRIBUTE_LIST m_list = nullptr;
};

static DWORD StartupFlags()
{
	DWORD flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_PROCESS_GROUP;
	BOOL inJob = FALSE;
	if (!IsProcessInJob(GetCurrentProcess(), nullptr, &inJob))
		throw windows::Error("Cannot inspect MCP connector job ownership");
	if (inJob)
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION information{};
		if (!QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &information, sizeof(information),
									   nullptr))
			throw windows::Error("Cannot inspect MCP connector job limits");
		const auto limits = information.BasicLimitInformation.LimitFlags;
		if (!(limits & JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK))
		{
			if (!(limits & JOB_OBJECT_LIMIT_BREAKAWAY_OK))
				throw std::runtime_error("The client job does not permit an independent simulator process");
			flags |= CREATE_BREAKAWAY_FROM_JOB;
		}
	}
	return flags;
}

void McpInstanceDiscovery::StartIfNeeded()
{
	windows::PrivateSecurity security(false, true);
	SetLastError(ERROR_SUCCESS);
	windows::Handle reservation(
		CreateMutexW(security.Attributes(), FALSE, MutexName(m_key, m_directory, security).c_str()));
	const auto existing = GetLastError();
	if (!reservation.Get())
		throw windows::Error("Cannot claim MCP app startup", existing);
	security.Check(reservation.Get(), SE_KERNEL_OBJECT, true);
	if (existing == ERROR_ALREADY_EXISTS)
		return;
	if (!std::filesystem::is_regular_file(m_executable))
		throw std::runtime_error("Selected MCP app executable is missing");
	std::error_code removal;
	std::filesystem::remove(m_directory / (m_key + ".json"), removal);
	if (removal)
		throw std::runtime_error("Cannot remove stale MCP discovery record: " + removal.message());
	SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
	windows::Handle nullFile(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
										 &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (nullFile.Get() == INVALID_HANDLE_VALUE)
		throw windows::Error("Cannot isolate MCP app standard streams");
	std::array<HANDLE, 2> handles{reservation.Get(), nullFile.Get()};
	McpProcessAttributes attributes(handles);
	auto command = windows::Quote(m_executable.native()) + L" --mcp-bootstrap-handle " +
				   std::to_wstring(reinterpret_cast<std::uintptr_t>(reservation.Get()));
	if (m_windowless)
		command += L" --windowless";
	if (command.size() >= 32767)
		throw std::invalid_argument("MCP app command line is too long");
	auto environment = ChildEnvironment(NewMcpIdentifier());
	STARTUPINFOEXW startup{};
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = nullFile.Get();
	startup.StartupInfo.hStdOutput = nullFile.Get();
	startup.StartupInfo.hStdError = nullFile.Get();
	startup.lpAttributeList = attributes.Get();
	PROCESS_INFORMATION process{};
	const auto flags = StartupFlags();
	if (!CreateProcessW(m_executable.c_str(), command.data(), nullptr, nullptr, TRUE, flags, environment.data(),
						m_executable.parent_path().c_str(), &startup.StartupInfo, &process))
		throw windows::Error("Cannot start the selected MCP app");
	m_child = process.hProcess;
	windows::Handle primaryThread(process.hThread);
}

std::optional<McpEndpoint> McpInstanceDiscovery::Read()
{
	if (m_child && WaitForSingleObject(m_child, 0) == WAIT_OBJECT_0)
	{
		DWORD code = 0;
		if (GetExitCodeProcess(m_child, &code) && !m_observed)
			m_startupError = "The selected simulator exited before MCP became ready (exit code " +
							 std::to_string(code) + "); start it manually to inspect the error";
		CloseHandle(m_child);
		m_child = nullptr;
	}
	windows::PrivateSecurity security(false);
	windows::Handle reservation(
		OpenMutexW(READ_CONTROL | SYNCHRONIZE, FALSE, MutexName(m_key, m_directory, security).c_str()));
	if (!reservation.Get())
	{
		if (GetLastError() == ERROR_FILE_NOT_FOUND)
			return {};
		throw windows::Error("Cannot check MCP app ownership");
	}
	security.Check(reservation.Get(), SE_KERNEL_OBJECT, true);
	const auto path = m_directory / (m_key + ".json");
	std::error_code error;
	const auto size = std::filesystem::file_size(path, error);
	if (error || size > 16384)
		return {};
	CheckPrivateFile(path);
	std::ifstream stream(path, std::ios::binary);
	if (!stream)
		return {};
	const auto value = Json::parse(stream, nullptr, false);
	if (value.is_discarded() || !value.is_object())
		return {};
	const auto protocol = value.find("connector_protocol");
	if (protocol != value.end() &&
		(!protocol->is_number_unsigned() || protocol->get<std::uint64_t>() != McpConnectorProtocolVersion))
	{
		m_startupError = "The selected app uses an unsupported MCP connector protocol; "
						 "install a compatible connector and reconnect this MCP entry";
		return {};
	}
	if (!value.contains("executable") || !value["executable"].is_string() ||
		NormalizedPath(Utf8Path(value["executable"].get<std::string>())) != NormalizedPath(m_executable) ||
		!value.contains("port") || !value["port"].is_number_unsigned() || !value.contains("token") ||
		!value["token"].is_string() || !value.contains("volume") || !value["volume"].is_string() ||
		!value.contains("index") || !value["index"].is_string())
		return {};
	const auto identity = FileIdentity(m_executable);
	if (!identity || value["volume"] != identity->volume || value["index"] != identity->index)
	{
		m_startupError = "The selected app executable changed while an older simulator instance is running; "
						 "close that instance and start the rebuilt app";
		return {};
	}
	const auto port = value["port"].get<std::uint64_t>();
	const auto token = value["token"].get<std::string>();
	if (!port || port > 65535 || token.size() != 64 || !std::ranges::all_of(token, [](unsigned char byte) {
			return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
		}))
		return {};
	m_observed = true;
	m_startupError.clear();
	return McpEndpoint{static_cast<std::uint16_t>(port), token};
}

const std::string& McpInstanceDiscovery::StartupError() const noexcept
{
	return m_startupError;
}

McpInstanceRegistration::McpInstanceRegistration(std::intptr_t inheritedHandle)
	: m_executable(McpOwnExecutable()), m_directory(PrivateDirectory()), m_key(SessionInstanceKey(m_executable))
{
	windows::PrivateSecurity security(false);
	const auto name = MutexName(m_key, m_directory, security);
	if (inheritedHandle >= 0)
	{
		windows::Handle inherited(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(inheritedHandle)));
		windows::Handle expected(OpenMutexW(READ_CONTROL | SYNCHRONIZE, FALSE, name.c_str()));
		if (!expected.Get())
			throw windows::Error("Cannot open MCP bootstrap reservation");
		security.Check(expected.Get(), SE_KERNEL_OBJECT, true);
		using CompareHandles = BOOL(WINAPI*)(HANDLE, HANDLE);
		const auto module = GetModuleHandleW(L"kernelbase.dll");
		const auto address = module ? GetProcAddress(module, "CompareObjectHandles") : nullptr;
		const auto compare = std::bit_cast<CompareHandles>(address);
		if (!compare || !compare(inherited.Get(), expected.Get()))
			throw std::runtime_error("Invalid MCP bootstrap reservation");
		if (!SetHandleInformation(inherited.Get(), HANDLE_FLAG_INHERIT, 0))
			throw windows::Error("Cannot protect MCP bootstrap reservation");
		HANDLE retained = nullptr;
		if (!DuplicateHandle(GetCurrentProcess(), inherited.Get(), GetCurrentProcess(), &retained, 0, FALSE,
							 DUPLICATE_SAME_ACCESS))
			throw windows::Error("Cannot retain MCP bootstrap reservation");
		m_lock = retained;
	}
	else
	{
		SetLastError(ERROR_SUCCESS);
		windows::Handle reservation(CreateMutexW(security.Attributes(), FALSE, name.c_str()));
		const auto result = GetLastError();
		if (!reservation.Get())
			throw windows::Error("Cannot reserve MCP app instance", result);
		security.Check(reservation.Get(), SE_KERNEL_OBJECT, true);
		if (result == ERROR_ALREADY_EXISTS)
			throw std::runtime_error("An MCP-enabled simulator for this executable is already running");
		HANDLE retained = nullptr;
		if (!DuplicateHandle(GetCurrentProcess(), reservation.Get(), GetCurrentProcess(), &retained, 0, FALSE,
							 DUPLICATE_SAME_ACCESS))
			throw windows::Error("Cannot retain MCP app reservation");
		m_lock = retained;
	}
}

McpInstanceRegistration::~McpInstanceRegistration()
{
	if (m_lock)
	{
		std::error_code ignored;
		std::filesystem::remove(m_directory / (m_key + ".json"), ignored);
		CloseHandle(m_lock);
	}
}

void McpInstanceRegistration::Publish(const McpEndpoint& endpoint)
{
	const auto identity = FileIdentity(m_executable);
	if (!identity)
		throw std::runtime_error("Cannot identify the running MCP app");
	const auto destination = m_directory / (m_key + ".json");
	const auto temporary = CreatePrivateMcpFile(destination);
	try
	{
		CheckPrivateFile(temporary);
		std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
		if (!stream)
			throw std::runtime_error("Cannot open MCP discovery record");
		stream << Json({{"connector_protocol", McpConnectorProtocolVersion},
						{"executable", PathText(m_executable)},
						{"port", endpoint.port},
						{"token", endpoint.token},
						{"process_id", GetCurrentProcessId()},
						{"volume", identity->volume},
						{"index", identity->index}})
					  .dump();
		stream.close();
		if (!stream)
			throw std::runtime_error("Cannot write MCP discovery record");
		CommitTemporaryFile(temporary, destination);
	}
	catch (...)
	{
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		throw;
	}
}
} // namespace simxr::app
