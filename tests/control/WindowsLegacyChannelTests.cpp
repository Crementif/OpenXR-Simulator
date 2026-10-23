#include "../TestSupport.h"
#include "control/legacy/WindowsLegacyCapture.h"
#include "json.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>

namespace
{
using namespace mcp;
namespace fs = std::filesystem;
class Directory
{
  public:
	Directory()
		: m_path(fs::temp_directory_path() /
				 ("simxr-windows-compatibility-" +
				  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) /
				 fs::path(u8"données-画面"))
	{
		fs::create_directories(m_path);
	}
	~Directory()
	{
		std::error_code ignored;
		fs::remove_all(m_path.parent_path(), ignored);
	}
	const fs::path& Path() const
	{
		return m_path;
	}

  private:
	fs::path m_path;
};
std::string Read(const fs::path& path)
{
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), {}};
}
void Command(const fs::path& directory, const char* filename, const std::string& document)
{
	const auto path = directory / filename;
	fs::remove(directory / (std::string(filename) + ".received"));

	const auto temporary = directory / "producer.tmp";
	{
		std::ofstream file(temporary, std::ios::binary);
		file << document;
		Require(file.good(), "Write compatibility fixture command");
	}
	std::error_code ignored;
	fs::remove(path, ignored);
	fs::rename(temporary, path);
	Eventually(
		[&]
		{
			return !fs::exists(path) && Read(directory / (std::string(filename) + ".received")) == "staged";
		},
		"Worker did not acknowledge independent typed staging");
}
void TestTypedStagingAndSeparateBoundaries()
{
	Directory directory;
	WindowsLegacyChannel files(directory.Path());
	Command(directory.Path(), "head_pose_command.json", R"({"x":2,"yaw":0.4,"roll":0.2})");
	Command(directory.Path(), "controller_pose_command.json", R"({"hand":0,"durationMs":125,"posX":-0.6,"buttonA":1})");
	Command(directory.Path(), "controller_hold_command.json", R"({"hand":-1,"grip":0.8,"stickX":-1,"roll":-0.4})");
	Command(directory.Path(), "wheel_sweep_command.json", R"({"enabled":true,"amplitudeDeg":37,"freqHz":0.2})");
	Command(directory.Path(), "screenshot_request.json", R"({"eye":"right","layer":"quad"})");
	Command(directory.Path(), "burst_command.json", R"({"frames":99,"pose":{"yaw":30}})");
	Command(directory.Path(), "projection_log_dump_request", "");
	auto requests = files.TakeRequests();
	Require(requests.screenshot && requests.capture.eye == "right" && requests.capture.layer == "quad" &&
				requests.burst.valid && requests.burst.frames == 64 && requests.burst.pose.valid &&
				requests.projectionLog,
			"Output requests were not prepared separately");
	auto values = files.TakeCommands();
	Require(values.head.valid && values.head.x == 2 && values.head.hasRoll && values.head.roll == .2f &&
				values.controllerPose.valid && values.controllerPose.durationMs == 125 &&
				!values.controllerPose.triggerSet && values.controllerPose.buttonA == 1 &&
				values.controllerHold.grip == .8f && values.controllerHold.roll == -.4f && values.wheel.enabled &&
				values.wheel.amplitudeDeg == 37,
			"Typed controller compatibility values changed");
	Require(!files.TakeCommands().head.valid && !files.TakeRequests().screenshot,
			"Consumed compatibility values were replayed");
}
void TestOpticsClearAndLatestCoalescing()
{
	Directory directory;
	WindowsLegacyChannel files(directory.Path());
	Command(directory.Path(), "head_pose_command.json", R"({"x":1})");
	Command(directory.Path(), "head_pose_command.json", R"({"x":4})");
	Command(directory.Path(), "fov_command.json", R"({"clear":false})");
	Command(directory.Path(), "ipd_command.json", R"({"ipd_mm":72})");
	Command(directory.Path(), "headset_profile_command.json", R"({"name":"quest3"})");
	Command(directory.Path(), "pose_sweep_command.json", R"({"enabled":true,"roll_amp_deg":12})");
	Command(directory.Path(), "anaglyph_command.json", R"({"enabled":true})");
	const auto values = files.TakeCommands();
	Require(values.head.x == 4 && values.fov.clear && values.ipd.ipdMeters > .0719f && values.ipd.ipdMeters < .0721f &&
				std::string(values.profile.name) == "quest3" && values.sweep.rollAmpDeg == 12 &&
				values.anaglyph.enabled,
			"Latest typed optics/legacy clear semantics changed");
}
void TestScreenshotOwnershipAndPublication()
{
	Directory directory;
	auto files = std::make_shared<WindowsLegacyChannel>(directory.Path());
	WindowsLegacyCapture capture(files);
	capture.SetFrame(42);
	capture.RequestScreenshot({"left\"\n", "projection\\"});
	std::vector<std::uint8_t> rgba{255, 0, 0, 7, 0, 255, 0, 9, 0, 0, 0, 0};
	Require(capture.SubmitPixels(rgba.data(), 2, 1, 12, false, 43), "Accept copied screenshot CPU bytes");
	std::fill(rgba.begin(), rgba.end(), 0);
	Require(!capture.ScreenshotRequested(), "Accepted image retained the old request");
	capture.Close();
	files->Close();
	const auto bitmap = Read(directory.Path() / "screenshot.bmp");
	Require(bitmap.size() == 62 && std::uint8_t(bitmap[56]) == 255 && std::uint8_t(bitmap[57]) == 7 &&
				std::uint8_t(bitmap[59]) == 255 && std::uint8_t(bitmap[61]) == 9,
			"Caller mutation changed retained image/alpha bytes");
	const auto status = Read(directory.Path() / "screenshot_status.json");
	Require(json::Object(status.c_str()).string("eye") == "left\"\n" &&
		json::Object(status.c_str()).string("layer") == "projection\\", "Screenshot status did not escape request text");
	Require(status.find("\"requestFrame\":42") != std::string::npos &&
				status.find("\"capturedFrame\":43") != std::string::npos,
			"Image completion status lost request/source identity");
	capture.RequestScreenshot();
	Require(!capture.SubmitPixels(rgba.data(), 2, 1, 12, false) && capture.ScreenshotRequested(),
			"Rejected CPU output consumed the pending request");
}
void TestAllCaptureUsesOneOutputTransaction()
{
	Directory directory;
	auto files = std::make_shared<WindowsLegacyChannel>(directory.Path());
	WindowsLegacyCapture capture(files);
	const std::array<std::uint8_t, 4> quad{255, 0, 0, 17};
	const std::array<std::uint8_t, 8> stereo{0, 255, 0, 7, 0, 0, 255, 9};
	capture.StoreQuad(quad.data(), 1, 1);
	capture.RequestScreenshot({"both", "all"});
	Require(capture.SubmitStereo(stereo.data(), nullptr, 2, 1), "Accept complete all-layer CPU output transaction");
	capture.Close();
	files->Close();
	const auto composite = Read(directory.Path() / "screenshot.bmp");
	const auto separateQuad = Read(directory.Path() / "screenshot_quad.bmp");
	Require(composite.size() == 62 && separateQuad.size() == 58 && static_cast<std::uint8_t>(composite[55]) == 255 &&
				static_cast<std::uint8_t>(separateQuad[56]) == 255 &&
				Read(directory.Path() / "screenshot_status.json").find("\"layer\":\"all\"") != std::string::npos &&
				files->TakeWrittenScreenshot() == std::optional<std::string>{"screenshot.bmp"},
			"All-layer completion metadata or copied pair changed");
}

void TestCaptureFailureAndQuadFrameIdentity()
{
	Directory directory;
	auto files = std::make_shared<WindowsLegacyChannel>(directory.Path());
	WindowsLegacyCapture capture(files);
	capture.SetFrame(41);
	capture.RequestScreenshot({"right", "quad"});
	capture.SetFrame(42);
	Require(capture.FailScreenshot("Unsupported \"quad\" format\nuse RGBA8"), "Accept explicit capture failure");
	Require(!capture.ScreenshotRequested(), "Accepted failure retires its pending request");
	Eventually(
		[&]
		{
			return fs::exists(directory.Path() / "screenshot_status.json");
		},
		"Failure status was not written");
	const auto failure = Read(directory.Path() / "screenshot_status.json");
	Require(failure.find("\"success\":false") != std::string::npos &&
				failure.find("Unsupported \\\"quad\\\" format\\u000ause RGBA8") != std::string::npos &&
				failure.find("\"requestFrame\":41") != std::string::npos &&
				!fs::exists(directory.Path() / "screenshot.bmp"),
			"Failure reports its source request without composite pixels");
	capture.RequestScreenshot({"right\"\n", "quad"});
	const std::array<std::uint8_t, 4> quad{255, 0, 0, 255};
	capture.StoreQuad(quad.data(), 1, 1, 43);
	capture.SetFrame(50);
	Require(capture.SubmitQuad(true, 43), "Accept completed raw quad frame");
	Eventually(
		[&]
		{
			return Read(directory.Path() / "screenshot_status.json").find("\"capturedFrame\":43") != std::string::npos;
		},
		"Delayed quad status was not published");
	Require(json::Object(Read(directory.Path() / "screenshot_status.json").c_str()).string("eye") == "right\"\n",
		"Quad status did not escape request text");
	capture.RequestScreenshot({"right", "quad"});
	Require(!capture.SubmitQuad(true, 43) && !capture.FailScreenshot("old map failed", 43) &&
				!capture.SubmitPixels(quad.data(), 1, 1, 4, false, 43) && capture.ScreenshotRequested(),
			"Old GPU pixels and failures cannot satisfy or retire a newer request");
	Require(!capture.SubmitQuad(true, 51), "A retained old quad cannot be relabeled with a newer frame");
	capture.StoreQuad(quad.data(), 1, 1, 51);
	Require(capture.SubmitQuad(true, 51), "A matching later frame satisfies the current request");
	capture.Close();
	files->Close();
	Require(Read(directory.Path() / "screenshot_status.json").find("\"capturedFrame\":51") != std::string::npos,
			"Delayed raw quad completion retains its actual captured frame");
	capture.RequestScreenshot();
	Require(!capture.FailScreenshot("closed") && capture.ScreenshotRequested(),
			"Closed output cannot silently consume a capture failure");
}

void TestAllCaptureDoesNotReuseAnOlderQuad()
{
	Directory directory;
	auto files = std::make_shared<WindowsLegacyChannel>(directory.Path());
	WindowsLegacyCapture capture(files);
	const std::array<std::uint8_t, 4> pixels{255, 0, 0, 255};
	capture.StoreQuad(pixels.data(), 1, 1, 43);
	capture.SetFrame(44);
	capture.RequestScreenshot({"both", "all"});
	Require(capture.SubmitPixels(pixels.data(), 1, 1, 4, false, 44), "Accept composite from the current frame");
	capture.Close();
	files->Close();
	Require(fs::exists(directory.Path() / "screenshot.bmp") && !fs::exists(directory.Path() / "screenshot_quad.bmp"),
			"All-layer capture does not attach retained quad pixels from an older frame");
}

void TestBurstAndProjectionRetention()
{
	Directory directory;
	auto files = std::make_shared<WindowsLegacyChannel>(directory.Path());
	WindowsLegacyCapture capture(files);
	ProjLogEntry projection;
	projection.frame = 7;
	projection.posX = 3;
	projection.rectW[0] = 2;
	capture.RecordProjection(projection);
	capture.DumpProjection();
	Require(capture.BeginBurst(2), "Reserve bounded editable burst");
	std::array<std::uint8_t, 8> pixels{1, 2, 3, 4, 5, 6, 7, 8};
	BurstFrameMeta metadata;
	metadata.frame = 8;
	metadata.headYaw = .5f;
	metadata.proj = capture.LastProjection();
	capture.BurstOnFrame(pixels.data(), 2, 1, 8, metadata);
	pixels.fill(255);
	metadata.frame = 9;
	capture.BurstOnFrame(pixels.data(), 2, 1, 8, metadata);
	Require(!capture.BurstActive(), "Completed burst remained active");
	capture.Close();
	files->Close();
	Require(Read(directory.Path() / "burst_done.json").find("\"count\":2") != std::string::npos &&
				Read(directory.Path() / "burst_manifest.json").find("\"head\":{\"yaw\":0.5") != std::string::npos &&
				Read(directory.Path() / "projection_log.json").find("\"x\":3") != std::string::npos &&
				std::uint8_t(Read(directory.Path() / "burst_000.bmp")[54]) == 1,
			"Burst CPU data or original projection metadata changed");
	Require(files->Status().activeBytes == 0 && files->Status().activeImages == 0, "Drained output retained budget");
}
void TestNumericFailureAndClassicJson()
{
	Directory directory;
	auto files = std::make_shared<WindowsLegacyChannel>(directory.Path());
	const auto commandPath = directory.Path() / "head_pose_command.json";
	{
		std::ofstream command(commandPath);
		command << "{\"x\":1e300}";
	}
	Eventually(
		[&]
		{
			return files->Status().errors != 0;
		},
		"Unrepresentable input was not rejected by preparation");
	Require(!files->TakeCommands().head.valid, "Malformed numeric input reached frame state");
	Command(directory.Path(), "head_pose_command.json", "{\"x\":3}");
	Require(files->TakeCommands().head.x == 3, "Failed preparation prevented a later explicit edit");
	struct CommaDecimal : std::numpunct<char>
	{
		char do_decimal_point() const override
		{
			return ',';
		}
	};
	class LocaleScope
	{
	  public:
		LocaleScope() : m_previous(std::locale::global(std::locale(std::locale::classic(), new CommaDecimal))) {}
		~LocaleScope()
		{
			std::locale::global(m_previous);
		}

	  private:
		std::locale m_previous;
	} locale;
	WindowsLegacyCapture capture(files);
	FrameStatus status;
	status.yaw = .25f;
	status.format = "format\"\\";
	status.state = "state\n";
	capture.PublishStatus(status);
	capture.Close();
	files->Close();
	const auto text = Read(directory.Path() / "runtime_status.json");
	Require(json::Object(text.c_str()).string("format") == status.format &&
		json::Object(text.c_str()).string("session_state") == status.state, "Runtime status did not escape text");
	Require(text.find("\"yaw\":0.25") != std::string::npos && text.find("\"timestamp\":") != std::string::npos,
			"Ambient locale or status preparation changed the legacy JSON contract");
	Require(!files->WaitForImageCapacity(0), "Closed worker allowed a new capacity waiter");
}

void TestCaptureArithmeticRejectsBeforeAllocation()
{
	Directory directory;
	auto files = std::make_shared<WindowsLegacyChannel>(directory.Path());
	WindowsLegacyCapture capture(files);
	capture.RequestScreenshot();
	const std::array<std::uint8_t, 8> pixels{};
	bool stereoRejected = false;
	try
	{
		capture.SubmitStereo(pixels.data(), pixels.data(), std::numeric_limits<std::uint32_t>::max(),
							 std::numeric_limits<std::uint32_t>::max());
	}
	catch (const std::invalid_argument&)
	{
		stereoRejected = true;
	}
	bool strideRejected = false;
	try
	{
		capture.SubmitPixels(pixels.data(), 1, 2, std::numeric_limits<std::size_t>::max(), true);
	}
	catch (const std::invalid_argument&)
	{
		strideRejected = true;
	}
	Require(stereoRejected && strideRejected && capture.ScreenshotRequested() && files->Status().activeImages == 0,
			"Overflowing capture dimensions/row offsets consumed admission or reached pixel allocation");
}

void TestFailureCapacityAndShutdown()
{
	Directory directory;
	auto files = std::make_shared<WindowsLegacyChannel>(directory.Path());
	fs::create_directory(directory.Path() / "screenshot.bmp");
	ImageOutput output;
	output.images.push_back({"screenshot.bmp", 1, 1, {1, 2, 3, 4}});
	output.statusFilename = "screenshot_status.json";
	output.status = "must not publish";
	Require(files->PublishImages(output), "Accept failing output fixture");
	Eventually(
		[&]
		{
			const auto status = files->Status();
			return status.errors && status.activeImages == 0;
		},
		"Failed image job did not release its CPU charge");
	for (const auto& entry : fs::directory_iterator(directory.Path()))
		Require(entry.path().filename().string().find(".tmp.") == std::string::npos,
				"Failed atomic publication left an open temporary file behind");
	Require(!fs::exists(directory.Path() / "screenshot_status.json"), "Failed bitmap claimed successful publication");
	Require(files->Publish(WindowsDocument::RuntimeStatus, "latest") && files->Log("joined output"),
			"Accept output before shutdown");
	files->Close();
	Require(Read(directory.Path() / "runtime_status.json") == "latest" && files->Status().closed &&
				!files->Publish(WindowsDocument::RuntimeStatus, "late"),
			"Shutdown did not drain accepted output/reject new work");
	ImageOutput invalid;
	invalid.images.push_back({"../escape.bmp", 1, 1, {1, 2, 3, 4}});
	bool rejected = false;
	try
	{
		files->PublishImages(invalid);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Image output escaped compatibility directory");
}
} // namespace
int main()
{
	try
	{
		TestTypedStagingAndSeparateBoundaries();
		TestOpticsClearAndLatestCoalescing();
		TestScreenshotOwnershipAndPublication();
		TestBurstAndProjectionRetention();
		TestAllCaptureUsesOneOutputTransaction();
		TestCaptureFailureAndQuadFrameIdentity();
		TestAllCaptureDoesNotReuseAnOlderQuad();
		TestFailureCapacityAndShutdown();
		TestCaptureArithmeticRejectsBeforeAllocation();
		TestNumericFailureAndClassicJson();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
