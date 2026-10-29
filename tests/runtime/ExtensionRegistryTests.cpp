#include "runtime/DispatchTable.h"
#include "../TestSupport.h"

#include <array>
#include <cstring>
#include <iostream>

void XRAPI_CALL Function() {}

int main()
{
	using namespace simxr;
	constexpr std::array<std::string_view, 1> baseCommands{"xrBaseExtension"};
	constexpr std::array<std::string_view, 1> featureCommands{"xrFeatureExtension"};
	const std::array<ExtensionDependency, 2> choices{{{"XR_TEST_base"}, {{}, XR_MAKE_VERSION(1, 1, 0)}}};
	const std::array<DependencyClause, 1> dependencies{{{choices}}};
	const std::array definitions{ExtensionDefinition{"XR_TEST_base", 3, baseCommands},
								 ExtensionDefinition{"XR_TEST_feature", 2, featureCommands, dependencies}};
	ExtensionRegistry registry(definitions);
	std::array<const char*, 1> requested{"XR_TEST_feature"};
	Require(registry.Validate(requested, XR_MAKE_VERSION(1, 0, 0)) == XR_ERROR_EXTENSION_NOT_PRESENT,
			"Missing dependency");
	Require(registry.Validate(requested, XR_MAKE_VERSION(1, 1, 0)) == XR_SUCCESS, "Core dependency alternative");
	std::array<const char*, 2> both{"XR_TEST_feature", "XR_TEST_base"};
	Require(registry.Validate(both, XR_MAKE_VERSION(1, 0, 0)) == XR_SUCCESS, "Explicit dependency");
	std::array<const char*, 1> missing{"XR_TEST_missing"};
	Require(registry.Validate(missing, XR_MAKE_VERSION(1, 1, 0)) == XR_ERROR_EXTENSION_NOT_PRESENT,
			"Unknown extension");
	std::uint32_t count;
	Require(registry.Enumerate(0, &count, nullptr) == XR_SUCCESS && count == 2, "Count enumeration");
	std::array<XrExtensionProperties, 2> properties{{{XR_TYPE_EXTENSION_PROPERTIES}, {XR_TYPE_EXTENSION_PROPERTIES}}};
	Require(registry.Enumerate(1, &count, properties.data()) == XR_ERROR_SIZE_INSUFFICIENT, "Capacity enumeration");
	int chainPayload = 0;
	properties[0].next = &chainPayload;
	Require(registry.Enumerate(2, &count, properties.data()) == XR_SUCCESS, "Extension enumeration");
	Require(properties[0].extensionVersion == 3 && properties[0].next == &chainPayload, "Revision or caller chain");
	std::array entries{DispatchEntry{"xrCreateInstance", Function, true}, DispatchEntry{"xrWaitFrame", Function},
					   DispatchEntry{"xrBaseExtension", Function}, DispatchEntry{"xrFeatureExtension", Function}};
	DispatchTable dispatch(entries, registry);
	Require(dispatch.CoversAdvertisedCommands(), "Missing advertised commands");
	EnabledExtensions disabled;
	EnabledExtensions enabled(both);
	PFN_xrVoidFunction function = Function;
	Require(dispatch.Resolve("xrBaseExtension", &disabled, function) == XR_ERROR_FUNCTION_UNSUPPORTED && !function,
			"Disabled extension dispatch");
	Require(dispatch.Resolve("xrBaseExtension", &enabled, function) == XR_SUCCESS && function == Function,
			"Enabled extension dispatch");
	Require(dispatch.Resolve("xrWaitFrame", nullptr, function) == XR_ERROR_HANDLE_INVALID && !function,
			"Null instance scope");
	Require(dispatch.Resolve("xrCreateInstance", nullptr, function) == XR_SUCCESS, "Global dispatch");
	std::cout << "Extension registry tests passed\n";
}
