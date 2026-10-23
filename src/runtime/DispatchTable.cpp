#include "runtime/DispatchTable.h"
#include "Pch.h"

namespace simxr
{
DispatchTable::DispatchTable(std::span<const DispatchEntry> entries, const ExtensionRegistry& extensions)
	: m_entries(entries), m_extensions(extensions)
{
}

XrResult DispatchTable::Resolve(std::string_view name, const EnabledExtensions* enabled,
								PFN_xrVoidFunction& function) const
{
	function = nullptr;
	auto entry = std::find_if(m_entries.begin(), m_entries.end(),
							  [&](const auto& value)
							  {
								  return value.name == name;
							  });
	if (entry == m_entries.end() || !entry->function)
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	if (!enabled && !entry->global)
		return XR_ERROR_HANDLE_INVALID;
	if (enabled && !m_extensions.CommandEnabled(name, *enabled))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	function = entry->function;
	return XR_SUCCESS;
}

bool DispatchTable::CoversAdvertisedCommands() const
{
	for (const auto& extension : m_extensions.Definitions())
		for (auto command : extension.commands)
			if (std::none_of(m_entries.begin(), m_entries.end(),
							 [&](const auto& entry)
							 {
								 return entry.name == command && entry.function;
							 }))
				return false;
	return true;
}
} // namespace simxr
