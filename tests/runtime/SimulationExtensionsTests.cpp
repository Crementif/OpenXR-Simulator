#include "runtime/SimulationExtensions.h"
#include "runtime/DispatchTable.h"
#include "../TestSupport.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <set>
#include <string_view>
#include <vector>

namespace
{
void XRAPI_PTR Stub()
{
}

struct ExpectedExtension
{
	const char* name;
	std::uint32_t revision;
	std::vector<std::string_view> commands;
};

struct Properties
{
	XrSystemProperties system{XR_TYPE_SYSTEM_PROPERTIES};
	XrSystemBodyTrackingPropertiesFB fb{XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB};
	XrSystemPropertiesBodyTrackingFullBodyMETA full{XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FULL_BODY_META};
	XrSystemBodyTrackingPropertiesBD bd{XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_BD};
	XrSystemPropertiesBodyTrackingCalibrationMETA calibration{XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_CALIBRATION_META};
	XrSystemPropertiesBodyTrackingFidelityMETA fidelity{XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FIDELITY_META};
	XrBaseOutStructure unknown{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT, nullptr};
	XrBaseOutStructure unknownTail{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT, nullptr};

	Properties()
	{
		system.next = &fb;
		fb.next = &unknown;
		unknown.next = reinterpret_cast<XrBaseOutStructure*>(&full);
		full.next = &bd;
		bd.next = &calibration;
		calibration.next = &fidelity;
		fidelity.next = &unknownTail;
		system.vendorId = 1234;
		std::strcpy(system.systemName, "untouched");
		Reset();
	}

	void Reset()
	{
		fb.supportsBodyTracking = full.supportsFullBodyTracking = bd.supportsBodyTracking =
			calibration.supportsHeightOverride = fidelity.supportsBodyTrackingFidelity = 42;
	}
};
}

int main()
{
	using namespace simxr;
	const std::array expected{
		ExpectedExtension{XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME, XR_EXT_eye_gaze_interaction_SPEC_VERSION, {}},
		ExpectedExtension{XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME, XR_HTCX_vive_tracker_interaction_SPEC_VERSION, {"xrEnumerateViveTrackerPathsHTCX"}},
		ExpectedExtension{XR_FB_BODY_TRACKING_EXTENSION_NAME, XR_FB_body_tracking_SPEC_VERSION,
			{"xrCreateBodyTrackerFB", "xrDestroyBodyTrackerFB", "xrLocateBodyJointsFB", "xrGetBodySkeletonFB"}},
		ExpectedExtension{XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME, XR_META_body_tracking_full_body_SPEC_VERSION, {}},
		ExpectedExtension{XR_BD_BODY_TRACKING_EXTENSION_NAME, XR_BD_body_tracking_SPEC_VERSION,
			{"xrCreateBodyTrackerBD", "xrDestroyBodyTrackerBD", "xrLocateBodyJointsBD"}},
		ExpectedExtension{XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME, XR_META_body_tracking_calibration_SPEC_VERSION,
			{"xrSuggestBodyTrackingCalibrationOverrideMETA", "xrResetBodyTrackingCalibrationMETA"}},
		ExpectedExtension{XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME, XR_META_body_tracking_fidelity_SPEC_VERSION,
			{"xrRequestBodyTrackingFidelityMETA"}}};
	const auto definitions = SimulationExtensions();
	Require(definitions.size() == expected.size(), "Simulation metadata extension count");
	ExtensionRegistry registry(definitions);
	std::vector<const char*> names;
	std::vector<DispatchEntry> entries;
	std::set<std::string_view> uniqueCommands;
	for (const auto& extension : expected)
	{
		names.push_back(extension.name);
		auto found = std::find_if(definitions.begin(), definitions.end(), [&](const auto& value) { return value.name == extension.name; });
		Require(found != definitions.end() && found->version == extension.revision, "Official extension metadata revision/name");
		Require(std::set<std::string_view>(found->commands.begin(), found->commands.end()) == std::set<std::string_view>(extension.commands.begin(), extension.commands.end()), "Extension command ownership");
		for (auto command : extension.commands)
		{
			Require(uniqueCommands.insert(command).second && registry.HasCommand(command), "Missing or duplicate simulation command");
			entries.push_back({command, Stub});
		}
	}
	Require(uniqueCommands.size() == 11 && !registry.HasCommand("xrGetBodyTrackingStateBD"), "Unsupported auxiliary commands advertised");
	EnabledExtensions all(names), none;
	Require(registry.Validate(names, XR_MAKE_VERSION(1, 0, 27)) == XR_SUCCESS, "Valid simulation extension set");
	for (const auto& extension : expected)
	{
		const std::array requested{extension.name};
		bool dependent = std::string_view(extension.name).starts_with("XR_META_");
		Require(registry.Validate(requested, XR_MAKE_VERSION(1, 0, 27)) == (dependent ? XR_ERROR_EXTENSION_NOT_PRESENT : XR_SUCCESS), "FB extension dependency");
		if (dependent)
		{
			const std::array complete{XR_FB_BODY_TRACKING_EXTENSION_NAME, extension.name};
			Require(registry.Validate(complete, XR_MAKE_VERSION(1, 0, 27)) == XR_SUCCESS, "Satisfied META dependency");
		}
	}
	const std::array fidelityWithoutCore{XR_FB_BODY_TRACKING_EXTENSION_NAME, XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME};
	Require(registry.Validate(fidelityWithoutCore, XR_MAKE_VERSION(0, 9, 0)) == XR_ERROR_EXTENSION_NOT_PRESENT, "Fidelity AND core1.0 dependency");
	std::uint32_t count = 0;
	Require(registry.Enumerate(0, &count, nullptr) == XR_SUCCESS && count == 7, "Metadata count enumeration");
	std::array<XrExtensionProperties, 7> properties{};
	for (auto& output : properties)
		output.type = XR_TYPE_EXTENSION_PROPERTIES;
	Require(registry.Enumerate(6, &count, properties.data()) == XR_ERROR_SIZE_INSUFFICIENT && count == 7, "Metadata enumeration capacity");
	Require(registry.Enumerate(7, &count, properties.data()) == XR_SUCCESS, "Metadata enumeration");
	for (const auto& property : properties)
	{
		auto found = std::find_if(expected.begin(), expected.end(), [&](const auto& value) { return std::string_view(property.extensionName) == value.name; });
		Require(found != expected.end() && property.extensionVersion == found->revision, "Enumerated official metadata");
	}
	DispatchTable dispatch(entries, registry);
	Require(dispatch.CoversAdvertisedCommands(), "Simulation dispatch coverage");
	for (const auto& entry : entries)
	{
		PFN_xrVoidFunction output = Stub;
		Require(dispatch.Resolve(entry.name, &none, output) == XR_ERROR_FUNCTION_UNSUPPORTED && !output, "Disabled simulation command resolved");
		Require(dispatch.Resolve(entry.name, &all, output) == XR_SUCCESS && output == Stub, "Enabled simulation command missing");
	}
	entries.pop_back();
	Require(!DispatchTable(entries, registry).CoversAdvertisedCommands(), "Missing advertised command coverage accepted");
	XrSystemEyeGazeInteractionPropertiesEXT gaze{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT, nullptr, 42};
	XrSystemProperties gazeSystem{XR_TYPE_SYSTEM_PROPERTIES, &gaze};
	Require(FillSimulationProperties(none, &gazeSystem) == XR_SUCCESS && gaze.supportsEyeGazeInteraction == 42, "Disabled gaze property consumed");
	Require(FillSimulationProperties(all, &gazeSystem) == XR_SUCCESS && gaze.supportsEyeGazeInteraction == XR_TRUE, "Enabled gaze capability");
	XrSystemEyeGazeInteractionPropertiesEXT duplicateGaze{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
	gaze.next = &duplicateGaze;
	Require(FillSimulationProperties(all, &gazeSystem) == XR_ERROR_VALIDATION_FAILURE, "Duplicate gaze properties");
	Require(FillSimulationProperties(none, &gazeSystem) == XR_SUCCESS, "Disabled gaze duplicate rejected");
	Properties chain;
	Require(FillSimulationProperties(all, &chain.system) == XR_SUCCESS && chain.fb.supportsBodyTracking && chain.full.supportsFullBodyTracking &&
		chain.bd.supportsBodyTracking && chain.calibration.supportsHeightOverride && chain.fidelity.supportsBodyTrackingFidelity, "Enabled simulation capabilities");
	Require(chain.system.next == &chain.fb && chain.fb.next == &chain.unknown && chain.unknown.next == reinterpret_cast<XrBaseOutStructure*>(&chain.full) &&
		chain.full.next == &chain.bd && chain.bd.next == &chain.calibration && chain.calibration.next == &chain.fidelity && chain.fidelity.next == &chain.unknownTail &&
		chain.system.vendorId == 1234 && std::strcmp(chain.system.systemName, "untouched") == 0, "System chain/base fields overwritten");
	chain.Reset();
	Require(FillSimulationProperties(none, &chain.system) == XR_SUCCESS && chain.fb.supportsBodyTracking == 42 && chain.full.supportsFullBodyTracking == 42 &&
		chain.bd.supportsBodyTracking == 42 && chain.calibration.supportsHeightOverride == 42 && chain.fidelity.supportsBodyTrackingFidelity == 42, "Disabled system structures consumed");
	const std::array fbName{XR_FB_BODY_TRACKING_EXTENSION_NAME};
	chain.Reset();
	Require(FillSimulationProperties(EnabledExtensions(fbName), &chain.system) == XR_SUCCESS && chain.fb.supportsBodyTracking == XR_TRUE &&
		chain.bd.supportsBodyTracking == 42 && chain.full.supportsFullBodyTracking == 42, "Independent extension structure gating");
	BodyTrackingCapabilities capabilities;
	capabilities.fb = false;
	Require(FillSimulationProperties(all, &chain.system, capabilities) == XR_SUCCESS && !chain.fb.supportsBodyTracking && !chain.full.supportsFullBodyTracking &&
		chain.bd.supportsBodyTracking && !chain.calibration.supportsHeightOverride && !chain.fidelity.supportsBodyTrackingFidelity, "FB capability dependency");
	capabilities = {};
	capabilities.fullBodyMETA = capabilities.calibrationMETA = capabilities.fidelityMETA = capabilities.bd = false;
	Require(FillSimulationProperties(all, &chain.system, capabilities) == XR_SUCCESS && chain.fb.supportsBodyTracking && !chain.full.supportsFullBodyTracking &&
		!chain.bd.supportsBodyTracking && !chain.calibration.supportsHeightOverride && !chain.fidelity.supportsBodyTrackingFidelity, "Independent feature capabilities");
	XrSystemBodyTrackingPropertiesFB duplicate{XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB};
	chain.unknownTail.next = reinterpret_cast<XrBaseOutStructure*>(&duplicate);
	chain.Reset();
	Require(FillSimulationProperties(all, &chain.system) == XR_ERROR_VALIDATION_FAILURE && chain.fb.supportsBodyTracking == 42, "Duplicate known system property partially mutated output");
	Require(FillSimulationProperties(none, &chain.system) == XR_SUCCESS && chain.fb.supportsBodyTracking == 42, "Duplicate unknown disabled property rejected");
	chain.unknownTail.next = &chain.unknown;
	Require(FillSimulationProperties(none, &chain.system) == XR_ERROR_VALIDATION_FAILURE, "Unknown cyclic system chain");
	chain.unknownTail.next = nullptr;
	Require(FillSimulationProperties(all, nullptr) == XR_ERROR_VALIDATION_FAILURE, "Null system properties");
	chain.system.type = XR_TYPE_INSTANCE_PROPERTIES;
	Require(FillSimulationProperties(all, &chain.system) == XR_ERROR_VALIDATION_FAILURE, "Wrong system properties type");
	chain.system.type = XR_TYPE_SYSTEM_PROPERTIES;
	chain.system.next = nullptr;
	Require(FillSimulationProperties(all, &chain.system) == XR_SUCCESS, "Empty system extension chain");
	std::cout << "Simulation extension metadata tests passed\n";
}
