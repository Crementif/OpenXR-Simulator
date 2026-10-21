#include "runtime/SimulationExtensions.h"
#include "runtime/validation/StructureChain.h"

#include <array>
#include <vector>

namespace simxr
{
std::span<const ExtensionDefinition> SimulationExtensions(SimulationExtensionCapabilities capabilities)
{
	static constexpr std::array<std::string_view, 1> trackerCommands{"xrEnumerateViveTrackerPathsHTCX"};
	static constexpr std::array<std::string_view, 4> fbCommands{"xrCreateBodyTrackerFB", "xrDestroyBodyTrackerFB",
																"xrLocateBodyJointsFB", "xrGetBodySkeletonFB"};
	static constexpr std::array<std::string_view, 3> bdCommands{"xrCreateBodyTrackerBD", "xrDestroyBodyTrackerBD",
																"xrLocateBodyJointsBD"};
	static constexpr std::array<std::string_view, 2> calibrationCommands{"xrSuggestBodyTrackingCalibrationOverrideMETA",
																		 "xrResetBodyTrackingCalibrationMETA"};
	static constexpr std::array<std::string_view, 1> fidelityCommands{"xrRequestBodyTrackingFidelityMETA"};
	static constexpr std::array fbDependency{ExtensionDependency{XR_FB_BODY_TRACKING_EXTENSION_NAME}};
	static constexpr std::array coreDependency{ExtensionDependency{{}, XR_MAKE_VERSION(1, 0, 0)}};
	static constexpr std::array dependsFB{DependencyClause{fbDependency}};
	static constexpr std::array dependsFidelity{DependencyClause{fbDependency}, DependencyClause{coreDependency}};
	static const std::array definitions{
		ExtensionDefinition{XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME, XR_EXT_eye_gaze_interaction_SPEC_VERSION, {}},
		ExtensionDefinition{XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME,
							XR_HTCX_vive_tracker_interaction_SPEC_VERSION, trackerCommands},
		ExtensionDefinition{XR_FB_BODY_TRACKING_EXTENSION_NAME, XR_FB_body_tracking_SPEC_VERSION, fbCommands},
		ExtensionDefinition{XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME,
							XR_META_body_tracking_full_body_SPEC_VERSION,
							{},
							dependsFB},
		ExtensionDefinition{XR_BD_BODY_TRACKING_EXTENSION_NAME, XR_BD_body_tracking_SPEC_VERSION, bdCommands},
		ExtensionDefinition{XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME,
							XR_META_body_tracking_calibration_SPEC_VERSION, calibrationCommands, dependsFB},
		ExtensionDefinition{XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME, XR_META_body_tracking_fidelity_SPEC_VERSION,
							fidelityCommands, dependsFidelity},
		ExtensionDefinition{XR_VARJO_QUAD_VIEWS_EXTENSION_NAME, XR_VARJO_quad_views_SPEC_VERSION, {}}};
	return std::span{definitions}.first(capabilities.quadViews ? definitions.size() : definitions.size() - 1);
}

XrResult FillSimulationProperties(const EnabledExtensions& enabled, XrSystemProperties* properties,
								  const BodyTrackingCapabilities& capabilities)
{
	if (!properties || properties->type != XR_TYPE_SYSTEM_PROPERTIES)
		return XR_ERROR_VALIDATION_FAILURE;
	std::vector<XrStructureType> known;
	if (enabled.Contains(XR_FB_BODY_TRACKING_EXTENSION_NAME))
		known.push_back(XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB);
	if (enabled.Contains(XR_BD_BODY_TRACKING_EXTENSION_NAME))
		known.push_back(XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_BD);
	if (enabled.Contains(XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME))
		known.push_back(XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FULL_BODY_META);
	if (enabled.Contains(XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME))
		known.push_back(XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_CALIBRATION_META);
	if (enabled.Contains(XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME))
		known.push_back(XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FIDELITY_META);
	if (enabled.Contains(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME))
		known.push_back(XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT);
	StructureChain chain(properties->next);
	auto result = chain.Validate(known);
	if (XR_FAILED(result))
		return result;
	for (auto type : known)
	{
		switch (type)
		{
		case XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT:
			if (auto output = const_cast<XrSystemEyeGazeInteractionPropertiesEXT*>(
					chain.Find<XrSystemEyeGazeInteractionPropertiesEXT>(type)))
				output->supportsEyeGazeInteraction = XR_TRUE;
			break;
		case XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB:
			if (auto output =
					const_cast<XrSystemBodyTrackingPropertiesFB*>(chain.Find<XrSystemBodyTrackingPropertiesFB>(type)))
				output->supportsBodyTracking = capabilities.fb ? XR_TRUE : XR_FALSE;
			break;
		case XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_BD:
			if (auto output =
					const_cast<XrSystemBodyTrackingPropertiesBD*>(chain.Find<XrSystemBodyTrackingPropertiesBD>(type)))
				output->supportsBodyTracking = capabilities.bd ? XR_TRUE : XR_FALSE;
			break;
		case XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FULL_BODY_META:
			if (auto output = const_cast<XrSystemPropertiesBodyTrackingFullBodyMETA*>(
					chain.Find<XrSystemPropertiesBodyTrackingFullBodyMETA>(type)))
				output->supportsFullBodyTracking = capabilities.fb && capabilities.fullBodyMETA ? XR_TRUE : XR_FALSE;
			break;
		case XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_CALIBRATION_META:
			if (auto output = const_cast<XrSystemPropertiesBodyTrackingCalibrationMETA*>(
					chain.Find<XrSystemPropertiesBodyTrackingCalibrationMETA>(type)))
				output->supportsHeightOverride = capabilities.fb && capabilities.calibrationMETA ? XR_TRUE : XR_FALSE;
			break;
		case XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FIDELITY_META:
			if (auto output = const_cast<XrSystemPropertiesBodyTrackingFidelityMETA*>(
					chain.Find<XrSystemPropertiesBodyTrackingFidelityMETA>(type)))
				output->supportsBodyTrackingFidelity =
					capabilities.fb && capabilities.fidelityMETA ? XR_TRUE : XR_FALSE;
			break;
		default:
			break;
		}
	}
	return XR_SUCCESS;
}
} // namespace simxr
