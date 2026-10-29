#include "json.h"
#include "Pch.h"

namespace json
{
static const char* SkipWhitespace(const char* cursor)
{
	while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r')
		++cursor;
	return cursor;
}

// Past the closing quote of the string opening at `cursor`, or nullptr if it never
// closes.
static const char* SkipString(const char* cursor)
{
	for (++cursor; *cursor; ++cursor)
	{
		if (*cursor == '\\' && cursor[1])
			++cursor;
		else if (*cursor == '"')
			return cursor + 1;
	}
	return nullptr;
}

// Past the value starting at `cursor`, whatever its type.
static const char* SkipValue(const char* cursor)
{
	cursor = SkipWhitespace(cursor);
	if (*cursor == '"')
		return SkipString(cursor);
	if (*cursor == '{' || *cursor == '[')
	{
		std::size_t depth = 0;
		while (*cursor)
		{
			if (*cursor == '"')
			{
				cursor = SkipString(cursor);
				if (!cursor)
					return nullptr;
				continue;
			}
			if (*cursor == '{' || *cursor == '[')
				++depth;
			else if ((*cursor == '}' || *cursor == ']') && --depth == 0)
				return cursor + 1;
			++cursor;
		}
		return nullptr;
	}
	// A number, true, false or null: runs until the member or its container
	// ends.
	while (*cursor && !std::strchr(",}] \t\n\r", *cursor))
		++cursor;
	return cursor;
}

static void AppendUtf8(std::string& output, unsigned codePoint)
{
	if (codePoint < 0x80)
	{
		output += static_cast<char>(codePoint);
	}
	else if (codePoint < 0x800)
	{
		output += static_cast<char>(0xC0 | (codePoint >> 6));
		output += static_cast<char>(0x80 | (codePoint & 0x3F));
	}
	else if (codePoint < 0x10000)
	{
		output += static_cast<char>(0xE0 | (codePoint >> 12));
		output += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
		output += static_cast<char>(0x80 | (codePoint & 0x3F));
	}
	else
	{
		output += static_cast<char>(0xF0 | (codePoint >> 18));
		output += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
		output += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
		output += static_cast<char>(0x80 | (codePoint & 0x3F));
	}
}

static bool Hex4(const char* cursor, unsigned& output)
{
	output = 0;
	for (int index = 0; index < 4; ++index)
	{
		char character = cursor[index];
		unsigned digit;
		if (character >= '0' && character <= '9')
			digit = static_cast<unsigned>(character - '0');
		else if (character >= 'a' && character <= 'f')
			digit = static_cast<unsigned>(character - 'a') + 10;
		else if (character >= 'A' && character <= 'F')
			digit = static_cast<unsigned>(character - 'A') + 10;
		else
			return false;
		output = output * 16 + digit;
	}
	return true;
}

// Unescapes the string opening at `cursor`. Anything malformed ends the string
// where it went wrong, keeping what came before it.
static std::string DecodeString(const char* cursor)
{
	std::string output;
	for (++cursor; *cursor && *cursor != '"';)
	{
		if (*cursor != '\\')
		{
			output += *cursor++;
			continue;
		}
		switch (*++cursor)
		{
		case '"':
			output += '"';
			++cursor;
			break;
		case '\\':
			output += '\\';
			++cursor;
			break;
		case '/':
			output += '/';
			++cursor;
			break;
		case 'b':
			output += '\b';
			++cursor;
			break;
		case 'f':
			output += '\f';
			++cursor;
			break;
		case 'n':
			output += '\n';
			++cursor;
			break;
		case 'r':
			output += '\r';
			++cursor;
			break;
		case 't':
			output += '\t';
			++cursor;
			break;
		case 'u':
		{
			unsigned codePoint;
			if (!Hex4(cursor + 1, codePoint))
				return output;
			cursor += 5;
			// A leading surrogate needs its trailing half from the next escape.
			if (codePoint >= 0xD800 && codePoint <= 0xDBFF && cursor[0] == '\\' && cursor[1] == 'u')
			{
				unsigned lowSurrogate;
				if (Hex4(cursor + 2, lowSurrogate) && lowSurrogate >= 0xDC00 && lowSurrogate <= 0xDFFF)
				{
					codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (lowSurrogate - 0xDC00);
					cursor += 6;
				}
			}
			if (codePoint >= 0xD800 && codePoint <= 0xDFFF)
				return output;
			AppendUtf8(output, codePoint);
			break;
		}
		default:
			return output;
		}
	}
	return output;
}

Object::Object(const char* text) : m_begin(text ? std::strchr(text, '{') : nullptr) {}

bool Object::Valid() const
{
	return m_begin != nullptr;
}

bool Object::Has(const char* key) const
{
	return Find(key) != nullptr;
}

std::string_view Object::Raw(const char* key) const
{
	const auto* value = Find(key);
	const auto* end = value ? SkipValue(value) : nullptr;
	return end ? std::string_view(value, static_cast<std::size_t>(end - value)) : std::string_view{};
}

double Object::Number(const char* key, double fallback) const
{
	const auto text = Raw(key);
	if (text.empty())
		return fallback;
	double value = fallback;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
	if (result.ec == std::errc::result_out_of_range)
		return std::numeric_limits<double>::quiet_NaN();
	return result.ec == std::errc{} ? value : fallback;
}

float Object::Number(const char* key, float fallback) const
{
	const double value = Number(key, static_cast<double>(fallback));
	if (!std::isfinite(value) || value < -std::numeric_limits<float>::max() ||
		value > std::numeric_limits<float>::max())
		return fallback;
	return static_cast<float>(value);
}

int Object::Number(const char* key, int fallback) const
{
	const double value = std::trunc(Number(key, static_cast<double>(fallback)));
	if (!std::isfinite(value) || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
		return fallback;
	return static_cast<int>(value);
}

bool Object::Boolean(const char* key, bool fallback) const
{
	const auto* value = Find(key);
	if (!value)
		return fallback;
	if (std::strncmp(value, "true", 4) == 0)
		return true;
	if (std::strncmp(value, "false", 5) == 0)
		return false;
	return fallback;
}

std::string Object::String(const char* key, const char* fallback) const
{
	const auto* value = Find(key);
	return value && *value == '"' ? DecodeString(value) : std::string(fallback);
}

Object Object::Child(const char* key) const
{
	Object child;
	const auto* value = Find(key);
	if (value && *value == '{')
		child.m_begin = value;
	return child;
}

const char* Object::Find(const char* key) const
{
	if (!m_begin || !key)
		return nullptr;
	const auto keyLength = std::strlen(key);
	auto* cursor = SkipWhitespace(m_begin + 1);
	while (*cursor && *cursor != '}')
	{
		if (*cursor != '"')
			return nullptr;
		const auto* name = cursor + 1;
		const auto* afterName = SkipString(cursor);
		if (!afterName)
			return nullptr;
		const auto nameLength = static_cast<std::size_t>(afterName - 1 - name);
		cursor = SkipWhitespace(afterName);
		if (*cursor != ':')
			return nullptr;
		const auto* value = SkipWhitespace(cursor + 1);
		if (nameLength == keyLength && std::strncmp(name, key, keyLength) == 0)
			return value;
		cursor = SkipValue(value);
		if (!cursor)
			return nullptr;
		cursor = SkipWhitespace(cursor);
		if (*cursor == ',')
			cursor = SkipWhitespace(cursor + 1);
	}
	return nullptr;
}
} // namespace json
