#include "automation/RigReport.h"

#include <algorithm>
#include <sstream>
#include <string_view>

namespace simxr::automation
{
namespace
{
std::string Escape(std::string_view text, bool xml = false)
{
	std::string result;
	for (const unsigned char c : text)
	{
		if (xml)
		{
			switch (c)
			{
			case '&':
				result += "&amp;";
				break;
			case '<':
				result += "&lt;";
				break;
			case '>':
				result += "&gt;";
				break;
			case '"':
				result += "&quot;";
				break;
			default:
				if (c >= 32 || c == '\n' || c == '\t')
					result += static_cast<char>(c);
			}
		}
		else
		{
			if (c == '"' || c == '\\')
			{
				result += '\\';
				result += static_cast<char>(c);
			}
			else if (c == '\n')
				result += "\\n";
			else if (c == '\r')
				result += "\\r";
			else if (c == '\t')
				result += "\\t";
			else if (c < 32)
			{
				const char hex[] = "0123456789abcdef";
				result += "\\u00";
				result += hex[c >> 4];
				result += hex[c & 15];
			}
			else
				result += static_cast<char>(c);
		}
	}
	return result;
}
const char* PhaseName(RigPhase phase)
{
	switch (phase)
	{
	case RigPhase::Idle:
		return "idle";
	case RigPhase::Running:
		return "running";
	case RigPhase::Paused:
		return "paused";
	case RigPhase::Complete:
		return "complete";
	case RigPhase::Cancelled:
		return "cancelled";
	case RigPhase::Failed:
		return "failed";
	}
	return "failed";
}
} // namespace
std::string SerializeRigJson(const RigSnapshot& result)
{
	std::ostringstream out;
	out << "{\"schema\":1,\"phase\":\"" << PhaseName(result.phase) << "\",\"seed\":\"" << result.seed
		<< "\",\"transactions\":" << result.transactions << ",\"captures\":" << result.captures << ",\"error\":\""
		<< Escape(result.error) << "\",\"checkpoints\":[";
	for (std::size_t i = 0; i < result.checkpoints.size(); ++i)
	{
		if (i)
			out << ',';
		const auto& checkpoint = result.checkpoints[i];
		out << "{\"name\":\"" << Escape(checkpoint.name) << "\",\"passed\":" << (checkpoint.passed ? "true" : "false")
			<< ",\"detail\":\"" << Escape(checkpoint.detail) << "\"}";
	}
	out << "],\"output\":[";
	for (std::size_t i = 0; i < result.output.size(); ++i)
	{
		if (i)
			out << ',';
		out << '"' << Escape(result.output[i]) << '"';
	}
	out << "]}";
	return out.str();
}
std::string SerializeRigJunit(const RigSnapshot& result)
{
	const auto failures = std::count_if(result.checkpoints.begin(), result.checkpoints.end(),
										[](const auto& item)
										{
											return !item.passed;
										});
	const bool executionFailure = result.phase != RigPhase::Complete;
	std::ostringstream out;
	out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?><testsuite name=\"simxr-rig\" tests=\""
		<< result.checkpoints.size() + 1 << "\" failures=\"" << failures + (executionFailure ? 1 : 0) << "\">";
	for (const auto& checkpoint : result.checkpoints)
	{
		out << "<testcase name=\"" << Escape(checkpoint.name, true) << "\">";
		if (!checkpoint.passed)
			out << "<failure message=\"" << Escape(checkpoint.detail, true) << "\"/>";
		out << "</testcase>";
	}
	out << "<testcase name=\"execution\">";
	if (executionFailure)
		out << "<failure message=\"" << Escape(result.error, true) << "\"/>";
	out << "</testcase></testsuite>";
	return out.str();
}
} // namespace simxr::automation
