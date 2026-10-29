#pragma once

#include <charconv>
#include <concepts>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <limits>
#include <string>
#include <string_view>

namespace simxr::probe
{
inline bool EnvOn(const char* name)
{
	const char* value = std::getenv(name);
	return value && *value && std::strcmp(value, "0") != 0;
}

template<std::integral T>
T EnvironmentCount(const char* name, T fallback, T minimum, T maximum = std::numeric_limits<T>::max())
{
	const char* value = std::getenv(name);
	if (!value)
		return fallback;
	const std::string_view number(value);
	T parsed = 0;
	const auto result = std::from_chars(number.data(), number.data() + number.size(), parsed);
	if (result.ec != std::errc{} || result.ptr != number.data() + number.size() || parsed < minimum || parsed > maximum)
		throw std::runtime_error(std::string("Invalid ") + name);
	return parsed;
}
} // namespace simxr::probe
