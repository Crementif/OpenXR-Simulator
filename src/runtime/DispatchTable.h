#pragma once

#include "runtime/ExtensionRegistry.h"

#include <openxr/openxr.h>
#include <span>
#include <string_view>

namespace simxr
{
struct DispatchEntry
{
	std::string_view name;
	PFN_xrVoidFunction function;
	bool global = false;
};

// Entries and the extension registry are borrowed for the table lifetime.
class DispatchTable
{
  public:
	DispatchTable(std::span<const DispatchEntry> entries, const ExtensionRegistry& extensions);
	XrResult Resolve(std::string_view name, const EnabledExtensions* enabled, PFN_xrVoidFunction& function) const;
	bool CoversAdvertisedCommands() const;

  private:
	std::span<const DispatchEntry> m_entries;
	const ExtensionRegistry& m_extensions;
};
} // namespace simxr
