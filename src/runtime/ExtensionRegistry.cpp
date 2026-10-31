#include "runtime/ExtensionRegistry.h"
#include "Pch.h"

namespace simxr
{
EnabledExtensions::EnabledExtensions(std::span<const char* const> requested, XrVersion apiVersion)
	: m_apiVersion(apiVersion)
{
	m_names.reserve(requested.size());
	for (auto name : requested)
	{
		if (!name)
			throw std::invalid_argument("Null extension name");
		m_names.emplace_back(name);
	}
}

bool EnabledExtensions::Core11() const noexcept
{
	return m_apiVersion >= XR_MAKE_VERSION(1, 1, 0);
}

bool EnabledExtensions::Contains(std::string_view name) const
{
	return std::find(m_names.begin(), m_names.end(), name) != m_names.end();
}

ExtensionRegistry::ExtensionRegistry(std::span<const ExtensionDefinition> definitions) : m_definitions(definitions)
{
	if (definitions.size() > std::numeric_limits<std::uint32_t>::max())
		throw std::invalid_argument("Too many extension definitions");
	for (std::size_t index = 0; index < definitions.size(); ++index)
	{
		const auto& definition = definitions[index];
		if (definition.name.empty() || definition.name.size() >= XR_MAX_EXTENSION_NAME_SIZE || definition.version == 0)
			throw std::invalid_argument("Invalid extension definition");
		for (std::size_t other = 0; other < index; ++other)
			if (definitions[other].name == definition.name)
				throw std::invalid_argument("Duplicate extension definition");
	}
}

XrResult ExtensionRegistry::Enumerate(std::uint32_t capacity, std::uint32_t* count,
									  XrExtensionProperties* properties) const
{
	if (!count || (capacity && !properties))
		return XR_ERROR_VALIDATION_FAILURE;
	*count = static_cast<std::uint32_t>(m_definitions.size());
	if (capacity == 0)
		return XR_SUCCESS;
	if (capacity < m_definitions.size())
		return XR_ERROR_SIZE_INSUFFICIENT;
	for (std::size_t index = 0; index < m_definitions.size(); ++index)
		if (properties[index].type != XR_TYPE_EXTENSION_PROPERTIES)
			return XR_ERROR_VALIDATION_FAILURE;
	for (std::size_t index = 0; index < m_definitions.size(); ++index)
	{
		const auto& definition = m_definitions[index];
		std::memcpy(properties[index].extensionName, definition.name.data(), definition.name.size());
		properties[index].extensionName[definition.name.size()] = '\0';
		properties[index].extensionVersion = definition.version;
	}
	return XR_SUCCESS;
}

XrResult ExtensionRegistry::Validate(std::span<const char* const> requested, XrVersion coreVersion) const
{
	for (auto name : requested)
	{
		if (!name)
			return XR_ERROR_VALIDATION_FAILURE;
		auto definition = std::find_if(m_definitions.begin(), m_definitions.end(),
									   [&](const auto& value) { return value.name == name; });
		if (definition == m_definitions.end())
			return XR_ERROR_EXTENSION_NOT_PRESENT;
	}
	EnabledExtensions enabled(requested);
	for (const auto& definition : m_definitions)
	{
		if (!enabled.Contains(definition.name))
			continue;
		for (const auto& clause : definition.dependencies)
		{
			bool satisfied =
				std::any_of(clause.alternatives.begin(), clause.alternatives.end(), [&](const auto& dependency) {
					return (!dependency.extension.empty() && enabled.Contains(dependency.extension)) ||
						   (dependency.coreVersion && coreVersion >= dependency.coreVersion);
				});
			if (!satisfied)
				return coreVersion >= XR_MAKE_VERSION(1, 1, 0) ? XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED
															   : XR_ERROR_EXTENSION_NOT_PRESENT;
		}
	}
	return XR_SUCCESS;
}

bool ExtensionRegistry::CommandEnabled(std::string_view command, const EnabledExtensions& enabled) const
{
	bool extensionCommand = false;
	for (const auto& definition : m_definitions)
	{
		if (std::find(definition.commands.begin(), definition.commands.end(), command) == definition.commands.end())
			continue;
		extensionCommand = true;
		if (enabled.Contains(definition.name))
			return true;
	}
	return !extensionCommand;
}

bool ExtensionRegistry::HasCommand(std::string_view command) const
{
	return std::any_of(m_definitions.begin(), m_definitions.end(), [&](const auto& definition) {
		return std::find(definition.commands.begin(), definition.commands.end(), command) != definition.commands.end();
	});
}

std::span<const ExtensionDefinition> ExtensionRegistry::Definitions() const
{
	return m_definitions;
}
} // namespace simxr
