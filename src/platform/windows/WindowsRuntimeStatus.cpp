#include "platform/windows/WindowsRuntimeStatus.h"
#include "Pch.h"
#include "control/legacy/WindowsLegacyFormat.h"

namespace simxr::windows
{
static void WriteJsonString(std::ostream& output, std::string_view text)
{
	static constexpr char s_hexDigits[] = "0123456789abcdef";
	output << '"';
	for (const unsigned char character : text)
	{
		if (character == '"' || character == '\\')
			output << '\\' << character;
		else if (character < 0x20)
			output << "\\u00" << s_hexDigits[character >> 4] << s_hexDigits[character & 0xF];
		else
			output << character;
	}
	output << '"';
}

WindowsRuntimeStatus::WindowsRuntimeStatus(std::shared_ptr<WindowsRuntimeFiles> files) : m_files(std::move(files))
{
	if (!m_files)
		throw std::invalid_argument("Runtime status requires an output worker");
}

void WindowsRuntimeStatus::PublishStatus(FrameStatus status)
{
	{
		std::lock_guard lock(m_mutex);
		const auto now = std::chrono::steady_clock::now();
		if (m_lastStatus != std::chrono::steady_clock::time_point{} &&
			now - m_lastStatus < std::chrono::milliseconds(500))
			return;
		m_lastStatus = now;
	}
	std::ostringstream output;
	output.imbue(std::locale::classic());
	output << "{\"timestamp\":\"" << mcp::LocalTimestamp() << "\",\"frame_count\":" << status.frame
		   << ",\"preview_width\":" << status.width << ",\"preview_height\":" << status.height << ",\"format\":";
	WriteJsonString(output, status.format);
	output << ",\"session_state\":";
	WriteJsonString(output, status.state);
	output << ",\"target_fps\":90,\"frame_time_ms\":11.1,"
		   << "\"head_tracking\":{\"position\":{\"x\":" << status.x << ",\"y\":" << status.y << ",\"z\":" << status.z
		   << "},\"yaw\":" << status.yaw << ",\"pitch\":" << status.pitch << ",\"roll\":" << status.roll << "}}";
	m_files->PublishStatus(output.str());
}
} // namespace simxr::windows
