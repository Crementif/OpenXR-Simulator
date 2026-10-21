#pragma once

#include <cstdint>
#include <openxr/openxr.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace simxr
{
struct ExtensionDependency
{
	std::string_view extension;
	XrVersion coreVersion = 0;
};

struct DependencyClause
{
	std::span<const ExtensionDependency> alternatives;
};

struct ExtensionDefinition
{
	std::string_view name;
	std::uint32_t version;
	std::span<const std::string_view> commands;
	std::span<const DependencyClause> dependencies{};
};

class EnabledExtensions
{
public:
	explicit EnabledExtensions(std::span<const char* const> requested = {});
	bool Contains(std::string_view name) const;

private:
	std::vector<std::string> m_names;
};

class ExtensionRegistry
{
public:
	explicit ExtensionRegistry(std::span<const ExtensionDefinition> definitions);
	XrResult Enumerate(std::uint32_t capacity, std::uint32_t* count, XrExtensionProperties* properties) const;
	XrResult Validate(std::span<const char* const> requested, XrVersion coreVersion) const;
	bool CommandEnabled(std::string_view command, const EnabledExtensions& enabled) const;
	bool HasCommand(std::string_view command) const;
	std::span<const ExtensionDefinition> Definitions() const;

private:
	std::span<const ExtensionDefinition> m_definitions;
};
}
