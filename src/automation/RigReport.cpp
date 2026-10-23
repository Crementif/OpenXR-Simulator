#include "automation/RigReport.h"

#include <algorithm>
#include <sstream>
#include <locale>
#include <string_view>

namespace simxr::automation
{
namespace
{
std::string Escape(std::string_view text, bool xml = false)
{
	std::string result;
	for (std::size_t offset = 0; offset < text.size(); ++offset)
	{
		const auto c = static_cast<unsigned char>(text[offset]);
		if (c >= 128)
		{
			const std::size_t length = c >= 0xc2 && c <= 0xdf ? 2 :
				c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
			unsigned codepoint = length ? c & (0x7f >> length) : 0;
			bool valid = length && length <= text.size() - offset;
			for (std::size_t index = 1; valid && index < length; ++index)
			{
				const auto next = static_cast<unsigned char>(text[offset + index]);
				valid = (next & 0xc0) == 0x80;
				codepoint = (codepoint << 6) | (next & 0x3f);
			}
			valid = valid && codepoint >= (length == 2 ? 0x80u : length == 3 ? 0x800u : 0x10000u) &&
				codepoint <= 0x10ffff && !(codepoint >= 0xd800 && codepoint <= 0xdfff);
			if (!valid)
				result += "\xef\xbf\xbd";
			else
			{
				if (xml && (codepoint == 0xfffe || codepoint == 0xffff))
					result += "\xef\xbf\xbd";
				else
					result.append(text.substr(offset, length));
				offset += length - 1;
			}
			continue;
		}
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
			case '\n':
				result += "&#10;";
				break;
			case '\r':
				result += "&#13;";
				break;
			case '\t':
				result += "&#9;";
				break;
			default:
				if (c >= 32)
					result += static_cast<char>(c);
				else
					result += "\xef\xbf\xbd";
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
	out.imbue(std::locale::classic());
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
	out << "],\"compatibility_deviations\":";
	if (!result.compatibilityDeviations)
		out << "null";
	else
	{
		out << '[';
		for (std::size_t i = 0; i < result.compatibilityDeviations->size(); ++i)
		{
			if (i)
				out << ',';
			out << '"' << Escape((*result.compatibilityDeviations)[i]) << '"';
		}
		out << ']';
	}
	out << '}';
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
	out.imbue(std::locale::classic());
	out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?><testsuite name=\"simxr-rig\" tests=\""
		<< result.checkpoints.size() + 1 << "\" failures=\"" << failures + (executionFailure ? 1 : 0) << "\">";
	out << "<properties><property name=\"compatibility_deviations\" value=\"";
	if (!result.compatibilityDeviations)
		out << "unknown";
	else if (result.compatibilityDeviations->empty())
		out << "none";
	else
		for (std::size_t i = 0; i < result.compatibilityDeviations->size(); ++i)
			out << (i ? "," : "") << Escape((*result.compatibilityDeviations)[i], true);
	out << "\"/></properties>";
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
