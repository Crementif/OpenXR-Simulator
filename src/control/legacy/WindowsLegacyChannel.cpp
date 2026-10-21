#include "control/legacy/WindowsLegacyChannel.h"
#include "control/legacy/WindowsLegacyFormat.h"
#include "json.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace mcp
{
enum class WindowsCommandFile
{
	HeadPose,
	Fov,
	Ipd,
	HeadsetProfile,
	Anaglyph,
	PoseSweep,
	ControllerPose,
	ControllerHold,
	WheelSweep,
	Screenshot,
	Burst,
	ProjectionLog
};
namespace
{
constexpr std::size_t MaxDocumentBytes = 1024 * 1024;
constexpr std::size_t MaxImageBytes = 512 * 1024 * 1024;
constexpr std::size_t MaxImageJobs = 4;
constexpr std::size_t MaxLogs = 64;
struct CommandFileDefinition
{
	WindowsCommandFile kind;
	const char* filename;
};
constexpr std::array CommandFiles{
	CommandFileDefinition{WindowsCommandFile::HeadPose, "head_pose_command.json"},
	CommandFileDefinition{WindowsCommandFile::Fov, "fov_command.json"},
	CommandFileDefinition{WindowsCommandFile::Ipd, "ipd_command.json"},
	CommandFileDefinition{WindowsCommandFile::HeadsetProfile, "headset_profile_command.json"},
	CommandFileDefinition{WindowsCommandFile::Anaglyph, "anaglyph_command.json"},
	CommandFileDefinition{WindowsCommandFile::PoseSweep, "pose_sweep_command.json"},
	CommandFileDefinition{WindowsCommandFile::ControllerPose, "controller_pose_command.json"},
	CommandFileDefinition{WindowsCommandFile::ControllerHold, "controller_hold_command.json"},
	CommandFileDefinition{WindowsCommandFile::WheelSweep, "wheel_sweep_command.json"},
	CommandFileDefinition{WindowsCommandFile::Screenshot, "screenshot_request.json"},
	CommandFileDefinition{WindowsCommandFile::Burst, "burst_command.json"},
	CommandFileDefinition{WindowsCommandFile::ProjectionLog, "projection_log_dump_request"}};
constexpr std::array DocumentFiles{"runtime_status.json", "projection_log.json", "command_ack.json"};

unsigned long ProcessId()
{
#ifdef _WIN32
	return GetCurrentProcessId();
#else
	return static_cast<unsigned long>(getpid());
#endif
}

bool Claim(const std::filesystem::path& source, const std::filesystem::path& destination)
{
#ifdef _WIN32
	if (MoveFileW(source.c_str(), destination.c_str()))
		return true;
	const auto error = GetLastError();
	if (error == ERROR_FILE_NOT_FOUND || error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
		return false;
	throw std::system_error(error, std::system_category(), "Claim Windows compatibility command");
#else
	if (syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(), 1) == 0)
		return true;
	if (errno == ENOENT || errno == EEXIST)
		return false;
	throw std::system_error(errno, std::generic_category(), "Claim compatibility fixture command");
#endif
}

void Replace(const std::filesystem::path& temporary, const std::filesystem::path& destination)
{
#ifdef _WIN32
	if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		throw std::system_error(GetLastError(), std::system_category(), "Publish compatibility output");
#else
	std::filesystem::rename(temporary, destination);
#endif
}

class AtomicOutput
{
  public:
	explicit AtomicOutput(std::filesystem::path destination)
		: m_destination(std::move(destination)),
		  m_temporary(m_destination.native() + std::filesystem::path(".tmp." + std::to_string(ProcessId())).native())
	{
		try
		{
			m_stream.exceptions(std::ios::badbit | std::ios::failbit);
			m_stream.open(m_temporary, std::ios::binary | std::ios::trunc);
		}
		catch (...)
		{
			Abort();
			throw;
		}
	}
	~AtomicOutput()
	{
		if (!m_committed)
			Abort();
	}
	void Write(std::span<const std::byte> bytes)
	{
		m_stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}
	void Commit()
	{
		m_stream.close();
		Replace(m_temporary, m_destination);
		m_committed = true;
	}

  private:
	void Abort() noexcept
	{
		// Windows cannot unlink an ordinary fstream's still-open file. Close
		// without throwing before removing even a partially acquired output.
		try
		{
			m_stream.exceptions(std::ios::goodbit);
			if (m_stream.is_open())
				m_stream.close();
		}
		catch (...)
		{
		}
		std::error_code ignored;
		std::filesystem::remove(m_temporary, ignored);
	}
	std::filesystem::path m_destination;
	std::filesystem::path m_temporary;
	std::ofstream m_stream;
	bool m_committed = false;
};

void WriteDocument(const std::filesystem::path& path, const std::string& document)
{
	AtomicOutput output(path);
	output.Write(std::as_bytes(std::span(document)));
	output.Commit();
}

void WriteBitmap(const std::filesystem::path& path, const BitmapOutput& image)
{
	std::array<std::byte, 54> header{};
	const auto store = [&](std::size_t offset, std::uint32_t value)
	{
		for (unsigned byte = 0; byte < 4; ++byte)
			header[offset + byte] = std::byte(value >> (byte * 8));
	};
	header[0] = std::byte{'B'};
	header[1] = std::byte{'M'};
	store(2, 54 + static_cast<std::uint32_t>(image.bgra.size()));
	store(10, 54);
	store(14, 40);
	store(18, image.width);
	store(22, 0u - image.height);
	header[26] = std::byte{1};
	header[28] = std::byte{32};
	store(34, static_cast<std::uint32_t>(image.bgra.size()));
	AtomicOutput output(path);
	output.Write(header);
	output.Write(std::as_bytes(std::span(image.bgra)));
	output.Commit();
}

std::string ReadDocument(const std::filesystem::path& path)
{
	if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)))
		throw std::invalid_argument("Compatibility command must be a regular file");
	const auto size = std::filesystem::file_size(path);
	if (size > 64 * 1024)
		throw std::invalid_argument("Compatibility command exceeds 64 KiB");
	std::ifstream input(path, std::ios::binary);
	input.exceptions(std::ios::badbit | std::ios::failbit);
	std::string document(static_cast<std::size_t>(size), '\0');
	input.read(document.data(), static_cast<std::streamsize>(document.size()));
	return document;
}
double FiniteNumber(const json::Object& document, const char* field, double fallback)
{
	const auto value = document.number(field, fallback);
	if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
		throw std::invalid_argument("Compatibility command requires finite representable numeric fields");
	return value;
}

HeadPoseCommand DecodeHeadPose(const std::string& text)
{
	HeadPoseCommand command;
	json::Object document(text.c_str());
	if (!document.valid())
		return command;

	command.valid = true;
	command.x = FiniteNumber(document, "x", 0.0f);
	command.y = FiniteNumber(document, "y", 1.7f);
	command.z = FiniteNumber(document, "z", 0.0f);
	command.yaw = FiniteNumber(document, "yaw", 0.0f);
	command.pitch = FiniteNumber(document, "pitch", 0.0f);
	command.hasRoll = document.has("roll");
	if (command.hasRoll)
		command.roll = FiniteNumber(document, "roll", 0.0f);
	return command;
}

FovCommand DecodeFov(const std::string& text)
{
	FovCommand command;
	json::Object document(text.c_str());
	if (!document.valid())
		return command;

	command.valid = true;
	if (document.has("clear"))
	{
		command.clear = true;
		return command;
	}
	auto parseEye = [&](const char* eyeKey, int eyeIndex)
	{
		json::Object eye = document.object(eyeKey);
		if (!eye.valid())
			return;
		command.angleLeft[eyeIndex] = FiniteNumber(eye, "aL", -1.0f);
		command.angleRight[eyeIndex] = FiniteNumber(eye, "aR", 1.0f);
		command.angleUp[eyeIndex] = FiniteNumber(eye, "aU", 1.0f);
		command.angleDown[eyeIndex] = FiniteNumber(eye, "aD", -1.0f);
	};
	parseEye("left", 0);
	parseEye("right", 1);
	return command;
}

IpdCommand DecodeIpd(const std::string& text)
{
	IpdCommand command;
	json::Object document(text.c_str());
	if (!document.valid())
		return command;

	command.valid = true;
	if (document.has("clear"))
	{
		command.clear = true;
		return command;
	}
	const float millimeters = FiniteNumber(document, "ipd_mm", 64.0f);
	command.ipdMeters = millimeters * 0.001f;
	return command;
}

HeadsetProfileCommand DecodeHeadsetProfile(const std::string& text)
{
	HeadsetProfileCommand command;
	std::string name = json::Object(text.c_str()).string("name");
	if (name.empty())
		return command;
	std::size_t nameLength = std::min(name.size(), sizeof(command.name) - 1);
	memcpy(command.name, name.data(), nameLength);
	command.name[nameLength] = 0;
	command.valid = true;
	return command;
}

PoseSweepCommand DecodePoseSweep(const std::string& text)
{
	PoseSweepCommand command;
	json::Object document(text.c_str());
	if (!document.valid())
		return command;

	command.valid = true;
	command.enabled = document.boolean("enabled", false);
	command.yawAmpDeg = FiniteNumber(document, "yaw_amp_deg", 30.0f);
	command.pitchAmpDeg = FiniteNumber(document, "pitch_amp_deg", 15.0f);
	command.rollAmpDeg = FiniteNumber(document, "roll_amp_deg", 15.0f);
	command.freqHz = FiniteNumber(document, "freq_hz", 0.25f);
	return command;
}

AnaglyphCommand DecodeAnaglyph(const std::string& text)
{
	AnaglyphCommand command;
	json::Object document(text.c_str());
	if (!document.valid())
		return command;

	command.valid = true;
	command.enabled = document.boolean("enabled", false);
	return command;
}

ControllerPoseCommand DecodeControllerPose(const std::string& text)
{
	ControllerPoseCommand command;
	json::Object document(text.c_str());
	if (!document.valid())
		return command;

	command.valid = true;
	command.hand = static_cast<int>(std::clamp(FiniteNumber(document, "hand", 1),
											   static_cast<double>(std::numeric_limits<int>::min()),
											   static_cast<double>(std::numeric_limits<int>::max())));
	command.posX = FiniteNumber(document, "posX", 0.2f);
	command.posY = FiniteNumber(document, "posY", -0.3f);
	command.posZ = FiniteNumber(document, "posZ", -0.4f);
	command.yaw = FiniteNumber(document, "yaw", 0.0f);
	command.pitch = FiniteNumber(document, "pitch", -0.3f);
	command.trigger = FiniteNumber(document, "trigger", -1.0f);
	command.triggerSet = (command.trigger >= 0.0f);
	if (!command.triggerSet)
		command.trigger = 0.0f;
	const auto button = FiniteNumber(document, "buttonA", -1);
	if (button < 0)
		command.buttonA = -1;
	else
		command.buttonA = static_cast<int>(button != 0);
	command.durationMs = FiniteNumber(document, "durationMs", 0.0f);
	return command;
}

ControllerHoldCommand DecodeControllerHold(const std::string& text)
{
	ControllerHoldCommand command;
	json::Object document(text.c_str());
	if (!document.valid())
		return command;

	command.valid = true;
	command.clear = document.boolean("clear", FiniteNumber(document, "clear", 0.0f) != 0.0f);
	command.hand = static_cast<int>(std::clamp(FiniteNumber(document, "hand", -1),
											   static_cast<double>(std::numeric_limits<int>::min()),
											   static_cast<double>(std::numeric_limits<int>::max())));
	command.trigger = FiniteNumber(document, "trigger", -1.0f);
	command.grip = FiniteNumber(document, "grip", -1.0f);
	command.primary = FiniteNumber(document, "primary", -1.0f);
	command.secondary = FiniteNumber(document, "secondary", -1.0f);
	command.menu = FiniteNumber(document, "menu", -1.0f);
	command.thumbClick = FiniteNumber(document, "thumbClick", -1.0f);
	command.stickX = FiniteNumber(document, "stickX", -2.0f);
	command.stickY = FiniteNumber(document, "stickY", -2.0f);
	command.yaw = FiniteNumber(document, "yaw", kHoldAngleUnset);
	command.pitch = FiniteNumber(document, "pitch", kHoldAngleUnset);
	command.roll = FiniteNumber(document, "roll", kHoldAngleUnset);
	return command;
}

WheelSweepCommand DecodeWheelSweep(const std::string& text)
{
	WheelSweepCommand command;
	json::Object document(text.c_str());
	if (!document.valid())
		return command;

	command.valid = true;
	command.enabled = document.boolean("enabled", FiniteNumber(document, "enabled", 1.0f) != 0.0f);
	command.hubX = FiniteNumber(document, "hubX", command.hubX);
	command.hubY = FiniteNumber(document, "hubY", command.hubY);
	command.hubZ = FiniteNumber(document, "hubZ", command.hubZ);
	command.radius = FiniteNumber(document, "radius", command.radius);
	command.tiltDeg = FiniteNumber(document, "tiltDeg", command.tiltDeg);
	command.amplitudeDeg = FiniteNumber(document, "amplitudeDeg", command.amplitudeDeg);
	command.freqHz = FiniteNumber(document, "freqHz", command.freqHz);
	command.grip = FiniteNumber(document, "grip", command.grip);
	command.spreadDeg = FiniteNumber(document, "spreadDeg", command.spreadDeg);
	return command;
}

BurstCommand DecodeBurst(const std::string& text)
{
	BurstCommand command;
	json::Object document(text.c_str());
	if (!document.valid())
		return command;
	command.valid = true;
	command.frames = static_cast<int>(std::clamp(FiniteNumber(document, "frames", 16), 1.0, 64.0));
	json::Object pose = document.object("pose");
	if (pose.valid())
	{
		command.pose.valid = true;
		command.pose.x = FiniteNumber(pose, "x", 0.0f);
		command.pose.y = FiniteNumber(pose, "y", 1.7f);
		command.pose.z = FiniteNumber(pose, "z", 0.0f);
		command.pose.yaw = FiniteNumber(pose, "yaw", 0.0f);
		command.pose.pitch = FiniteNumber(pose, "pitch", 0.0f);
		command.pose.hasRoll = pose.has("roll");
		if (command.pose.hasRoll)
			command.pose.roll = FiniteNumber(pose, "roll", 0.0f);
	}
	return command;
}
} // namespace

WindowsLegacyChannel::WindowsLegacyChannel(std::filesystem::path directory) : m_directory(std::move(directory))
{
	std::filesystem::create_directories(m_directory);
	m_images.reserve(MaxImageJobs);
	m_logs.reserve(MaxLogs);
	m_worker = std::thread(
		[this]
		{
			Run();
		});
}
WindowsLegacyChannel::~WindowsLegacyChannel()
{
	Close();
}
WindowsInputCommands WindowsLegacyChannel::TakeCommands()
{
	std::lock_guard lock(m_mutex);
	return std::exchange(m_commands, {});
}
WindowsOutputRequests WindowsLegacyChannel::TakeRequests()
{
	WindowsOutputRequests requests;
	{
		std::lock_guard lock(m_mutex);
		std::swap(requests, m_requests);
	}
	return requests;
}
std::optional<std::string> WindowsLegacyChannel::TakeWrittenScreenshot()
{
	std::optional<std::string> written;
	{
		std::lock_guard lock(m_mutex);
		written.swap(m_writtenScreenshot);
	}
	return written;
}
bool WindowsLegacyChannel::Publish(WindowsDocument kind, std::string document)
{
	const auto index = static_cast<std::size_t>(kind);
	if (index >= m_documents.size() || document.size() > MaxDocumentBytes)
		return false;
	{
		std::lock_guard lock(m_mutex);
		if (m_stopping)
			return false;
		m_documents[index].swap(document);
		m_documentReady[index] = true;
	}
	m_changed.notify_all();
	return true;
}
bool WindowsLegacyChannel::PublishImages(ImageOutput& output)
{
	if (output.images.size() > 64 || output.status.size() > MaxDocumentBytes ||
		output.manifest.size() > MaxDocumentBytes)
		throw std::invalid_argument("Compatibility image output metadata exceeds its bound");
	if (!output.statusFilename.empty() && output.statusFilename != "screenshot_status.json" &&
		output.statusFilename != "burst_done.json" && output.statusFilename != "command_ack.json")
		throw std::invalid_argument("Unknown compatibility image status file");
	std::size_t bytes = 0;
	for (const auto& image : output.images)
	{
		const auto pixels = std::uint64_t(image.width) * image.height;
		if (!pixels || pixels > MaxImageBytes / 4 || image.bgra.size() != pixels * 4 ||
			image.bgra.size() > MaxImageBytes - bytes)
			throw std::invalid_argument("Invalid compatibility bitmap extent or byte count");
		if (std::filesystem::path(image.filename).filename() != image.filename)
			throw std::invalid_argument("Compatibility image filename must be local");
		bytes += image.bgra.size();
	}
	{
		std::lock_guard lock(m_mutex);
		if (m_stopping || m_status.activeImages == MaxImageJobs || bytes > MaxImageBytes - m_status.activeBytes)
			return false;
		m_images.push_back(std::move(output));
		++m_status.activeImages;
		m_status.activeBytes += bytes;
	}
	m_changed.notify_all();
	return true;
}
bool WindowsLegacyChannel::WaitForImageCapacity(std::size_t bytes)
{
	if (bytes > MaxImageBytes)
		return false;
	std::unique_lock lock(m_mutex);
	m_changed.wait(lock,
				   [this, bytes]
				   {
					   return m_stopping ||
							  (m_status.activeImages < MaxImageJobs && bytes <= MaxImageBytes - m_status.activeBytes);
				   });
	return !m_stopping;
}
bool WindowsLegacyChannel::Log(std::string message)
{
	if (message.size() > 4096)
		message.resize(4096);
	{
		std::lock_guard lock(m_mutex);
		if (m_stopping)
			return false;
		if (m_logs.size() == MaxLogs)
		{
			++m_status.droppedLogs;
			return false;
		}
		m_logs.push_back(std::move(message));
	}
	m_changed.notify_all();
	return true;
}
void WindowsLegacyChannel::Acknowledge(std::string_view command, bool success)
{
	Publish(WindowsDocument::CommandAck, "{\"timestamp\":\"" + detail::LocalTimestamp() + "\",\"command\":\"" +
											 std::string(command) + "\",\"success\":" + (success ? "true" : "false") +
											 "}");
}
WindowsChannelStatus WindowsLegacyChannel::Status() const
{
	std::lock_guard lock(m_mutex);
	return m_status;
}
void WindowsLegacyChannel::Close()
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
void WindowsLegacyChannel::RecordError(std::string_view error) noexcept
{
	try
	{
		std::lock_guard lock(m_mutex);
		++m_status.errors;
		m_status.detail.assign(error);
	}
	catch (...)
	{
	}
}
void WindowsLegacyChannel::Stage(WindowsCommandFile command, const std::string& document)
{
	// Decoding and temporary allocations complete before the mailbox lock.
	switch (command)
	{
	case WindowsCommandFile::HeadPose:
	{
		auto value = DecodeHeadPose(document);
		std::lock_guard lock(m_mutex);
		m_commands.head = value;
		break;
	}
	case WindowsCommandFile::Fov:
	{
		auto value = DecodeFov(document);
		std::lock_guard lock(m_mutex);
		m_commands.fov = value;
		break;
	}
	case WindowsCommandFile::Ipd:
	{
		auto value = DecodeIpd(document);
		std::lock_guard lock(m_mutex);
		m_commands.ipd = value;
		break;
	}
	case WindowsCommandFile::HeadsetProfile:
	{
		auto value = DecodeHeadsetProfile(document);
		std::lock_guard lock(m_mutex);
		m_commands.profile = value;
		break;
	}
	case WindowsCommandFile::Anaglyph:
	{
		auto value = DecodeAnaglyph(document);
		std::lock_guard lock(m_mutex);
		m_commands.anaglyph = value;
		break;
	}
	case WindowsCommandFile::PoseSweep:
	{
		auto value = DecodePoseSweep(document);
		std::lock_guard lock(m_mutex);
		m_commands.sweep = value;
		break;
	}
	case WindowsCommandFile::ControllerPose:
	{
		auto value = DecodeControllerPose(document);
		std::lock_guard lock(m_mutex);
		m_commands.controllerPose = value;
		break;
	}
	case WindowsCommandFile::ControllerHold:
	{
		auto value = DecodeControllerHold(document);
		std::lock_guard lock(m_mutex);
		m_commands.controllerHold = value;
		break;
	}
	case WindowsCommandFile::WheelSweep:
	{
		auto value = DecodeWheelSweep(document);
		std::lock_guard lock(m_mutex);
		m_commands.wheel = value;
		break;
	}
	case WindowsCommandFile::Screenshot:
	{
		json::Object parsed(document.c_str());
		ScreenshotRequest request{parsed.string("eye", "both"), parsed.string("layer", "projection")};
		std::lock_guard lock(m_mutex);
		std::swap(m_requests.capture, request);
		m_requests.screenshot = true;
		break;
	}
	case WindowsCommandFile::Burst:
	{
		auto value = DecodeBurst(document);
		std::lock_guard lock(m_mutex);
		m_requests.burst = value;
		break;
	}
	case WindowsCommandFile::ProjectionLog:
	{
		std::lock_guard lock(m_mutex);
		m_requests.projectionLog = true;
		break;
	}
	default:
		throw std::invalid_argument("Unknown compatibility command mailbox");
	}
}
void WindowsLegacyChannel::ReadCommand(WindowsCommandFile command, std::string_view filename)
{
	const auto path = m_directory / filename;
	const auto claimed = m_directory / (std::string(filename) + ".pending." + std::to_string(ProcessId()));
	bool staged = false;
	try
	{
		if (!std::filesystem::exists(claimed) && !Claim(path, claimed))
			return;
		const auto document = ReadDocument(claimed);
		Stage(command, document);
		staged = true;
		WriteDocument(m_directory / (std::string(filename) + ".received"), "staged");
		std::filesystem::remove(claimed);
	}
	catch (const std::exception& error)
	{
		RecordError(error.what());
		if (staged)
		{
			std::error_code ignored;
			std::filesystem::remove(claimed, ignored);
			return;
		}
		try
		{
			if (Claim(claimed, path) || std::filesystem::exists(path))
			{
				std::error_code ignored;
				std::filesystem::remove(claimed, ignored);
			}
		}
		catch (const std::exception& restore)
		{
			RecordError(restore.what());
		}
	}
}
void WindowsLegacyChannel::Scan()
{
	for (const auto& command : CommandFiles)
		ReadCommand(command.kind, command.filename);
}
void WindowsLegacyChannel::WriteImages(const ImageOutput& output)
{
	if (output.resetBurst)
	{
		for (unsigned index = 0; index < 64; ++index)
		{
			std::ostringstream filename;
			filename.imbue(std::locale::classic());
			filename << "burst_" << std::setw(3) << std::setfill('0') << index << ".bmp";
			std::filesystem::remove(m_directory / filename.str());
		}
		std::filesystem::remove(m_directory / "burst_done.json");
		std::filesystem::remove(m_directory / "burst_manifest.json");
	}
	for (const auto& image : output.images)
		WriteBitmap(m_directory / image.filename, image);
	if (!output.manifest.empty())
		WriteDocument(m_directory / "burst_manifest.json", output.manifest);
	if (!output.statusFilename.empty())
		WriteDocument(m_directory / output.statusFilename, output.status);

	const auto written = std::find_if(output.images.begin(), output.images.end(),
									  [](const auto& image)
									  {
										  return image.filename == "screenshot.bmp";
									  });
	const auto quad = std::find_if(output.images.begin(), output.images.end(),
								   [](const auto& image)
								   {
									   return image.filename == "screenshot_quad.bmp";
								   });
	const auto screenshot = written != output.images.end() ? written : quad;
	if (screenshot != output.images.end())
	{
		std::optional<std::string> filename(screenshot->filename);
		std::lock_guard lock(m_mutex);
		m_writtenScreenshot.swap(filename);
	}
}
void WindowsLegacyChannel::Run() noexcept
{
	try
	{
		std::vector<ImageOutput> images;
		std::vector<std::string> logs;
		images.reserve(MaxImageJobs);
		logs.reserve(MaxLogs);
		auto nextScan = std::chrono::steady_clock::now();
		for (;;)
		{
			const auto now = std::chrono::steady_clock::now();
			if (now >= nextScan)
			{
				Scan();
				nextScan = now + std::chrono::milliseconds(100);
			}
			std::array<std::string, 3> documents;
			std::array<bool, 3> ready;
			bool stopping;
			{
				std::lock_guard lock(m_mutex);
				documents.swap(m_documents);
				ready = std::exchange(m_documentReady, {});
				images.swap(m_images);
				logs.swap(m_logs);
				stopping = m_stopping;
			}
			for (std::size_t index = 0; index < ready.size(); ++index)
				if (ready[index])
					try
					{
						WriteDocument(m_directory / DocumentFiles[index], documents[index]);
					}
					catch (const std::exception& error)
					{
						RecordError(error.what());
					}
			for (auto& output : images)
			{
				try
				{
					WriteImages(output);
				}
				catch (const std::exception& error)
				{
					RecordError(error.what());
				}
				std::size_t bytes = 0;
				for (const auto& image : output.images)
					bytes += image.bgra.size();
				output.images.clear();
				std::lock_guard lock(m_mutex);
				--m_status.activeImages;
				m_status.activeBytes -= bytes;
				m_changed.notify_all();
			}
			if (!logs.empty())
				try
				{
					std::ofstream file(m_directory / ("openxr_simulator." + std::to_string(ProcessId()) + ".log"),
									   std::ios::app);
					file.exceptions(std::ios::badbit | std::ios::failbit);
					for (const auto& line : logs)
						file << line << '\n';
				}
				catch (const std::exception& error)
				{
					RecordError(error.what());
				}
			images.clear();
			logs.clear();
			if (stopping)
				break;
			std::unique_lock lock(m_mutex);
			m_changed.wait_until(lock, nextScan,
								 [this]
								 {
									 return m_stopping || !m_images.empty() || !m_logs.empty() ||
											std::any_of(m_documentReady.begin(), m_documentReady.end(),
														[](bool ready)
														{
															return ready;
														});
								 });
		}
	}
	catch (const std::exception& error)
	{
		RecordError(error.what());
	}
	catch (...)
	{
	}
	std::lock_guard lock(m_mutex);
	m_stopping = true;
	m_status.closed = true;
	m_changed.notify_all();
}
} // namespace mcp
