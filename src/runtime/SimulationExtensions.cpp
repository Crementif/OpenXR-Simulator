#include "runtime/SimulationExtensions.h"
#include "Pch.h"
#include "runtime/validation/StructureChain.h"

namespace simxr
{
std::span<const ExtensionDefinition> SimulationExtensions(SimulationExtensionCapabilities capabilities)
{
	static constexpr std::array<std::string_view, 1> s_trackerCommands{"xrEnumerateViveTrackerPathsHTCX"};
	static constexpr std::array<std::string_view, 4> s_fbCommands{"xrCreateBodyTrackerFB", "xrDestroyBodyTrackerFB",
																  "xrLocateBodyJointsFB", "xrGetBodySkeletonFB"};
	static constexpr std::array<std::string_view, 3> s_bdCommands{"xrCreateBodyTrackerBD", "xrDestroyBodyTrackerBD",
																  "xrLocateBodyJointsBD"};
	static constexpr std::array<std::string_view, 2> s_calibrationCommands{
		"xrSuggestBodyTrackingCalibrationOverrideMETA", "xrResetBodyTrackingCalibrationMETA"};
	static constexpr std::array<std::string_view, 1> s_fidelityCommands{"xrRequestBodyTrackingFidelityMETA"};
	static constexpr std::array<std::string_view, 3> s_refreshCommands{
		"xrEnumerateDisplayRefreshRatesFB", "xrGetDisplayRefreshRateFB", "xrRequestDisplayRefreshRateFB"};
	static constexpr std::array s_fbDependency{ExtensionDependency{XR_FB_BODY_TRACKING_EXTENSION_NAME}};
	static constexpr std::array s_coreDependency{ExtensionDependency{{}, XR_MAKE_VERSION(1, 0, 0)}};
	static constexpr std::array s_dependsFB{DependencyClause{s_fbDependency}};
	static constexpr std::array s_dependsFidelity{DependencyClause{s_fbDependency}, DependencyClause{s_coreDependency}};
	static constexpr std::array s_quadDependency{ExtensionDependency{XR_VARJO_QUAD_VIEWS_EXTENSION_NAME},
												 ExtensionDependency{{}, XR_MAKE_VERSION(1, 1, 0)}};
	static constexpr std::array s_dependsQuad{DependencyClause{s_quadDependency}};
	static const std::array s_definitions{
		ExtensionDefinition{XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME, XR_EXT_eye_gaze_interaction_SPEC_VERSION, {}},
		ExtensionDefinition{XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME,
							XR_HTCX_vive_tracker_interaction_SPEC_VERSION, s_trackerCommands},
		ExtensionDefinition{XR_FB_BODY_TRACKING_EXTENSION_NAME, XR_FB_body_tracking_SPEC_VERSION, s_fbCommands},
		ExtensionDefinition{XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME,
							XR_META_body_tracking_full_body_SPEC_VERSION,
							{},
							s_dependsFB},
		ExtensionDefinition{XR_BD_BODY_TRACKING_EXTENSION_NAME, XR_BD_body_tracking_SPEC_VERSION, s_bdCommands},
		ExtensionDefinition{XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME,
							XR_META_body_tracking_calibration_SPEC_VERSION, s_calibrationCommands, s_dependsFB},
		ExtensionDefinition{XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME, XR_META_body_tracking_fidelity_SPEC_VERSION,
							s_fidelityCommands, s_dependsFidelity},
		ExtensionDefinition{XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, XR_FB_display_refresh_rate_SPEC_VERSION,
							s_refreshCommands},
		ExtensionDefinition{XR_VARJO_QUAD_VIEWS_EXTENSION_NAME, XR_VARJO_quad_views_SPEC_VERSION, {}},
		ExtensionDefinition{
			XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME, XR_VARJO_foveated_rendering_SPEC_VERSION, {}, s_dependsQuad}};
	const std::size_t additional = capabilities.quadViews ? (capabilities.foveatedRendering ? 2 : 1) : 0;
	return std::span{s_definitions}.first(s_definitions.size() - 2 + additional);
}

XrResult FillSimulationProperties(const EnabledExtensions& enabled, XrSystemProperties* properties,
								  const BodyTrackingCapabilities& capabilities)
{
	if (!properties || properties->type != XR_TYPE_SYSTEM_PROPERTIES)
		return XR_ERROR_VALIDATION_FAILURE;
	std::array<XrStructureType, 7> known{};
	std::size_t knownCount = 0;
	if (enabled.Contains(XR_FB_BODY_TRACKING_EXTENSION_NAME))
		known[knownCount++] = XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB;
	if (enabled.Contains(XR_BD_BODY_TRACKING_EXTENSION_NAME))
		known[knownCount++] = XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_BD;
	if (enabled.Contains(XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME))
		known[knownCount++] = XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FULL_BODY_META;
	if (enabled.Contains(XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME))
		known[knownCount++] = XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_CALIBRATION_META;
	if (enabled.Contains(XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME))
		known[knownCount++] = XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FIDELITY_META;
	if (enabled.Contains(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME))
		known[knownCount++] = XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT;
	if (enabled.Contains(XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME))
		known[knownCount++] = XR_TYPE_SYSTEM_FOVEATED_RENDERING_PROPERTIES_VARJO;
	StructureChain chain(properties->next);
	const auto knownTypes = std::span(known).first(knownCount);
	auto result = chain.Validate(knownTypes);
	if (XR_FAILED(result))
		return result;
	for (auto type : knownTypes)
	{
		switch (type)
		{
		case XR_TYPE_SYSTEM_FOVEATED_RENDERING_PROPERTIES_VARJO:
			if (auto output = const_cast<XrSystemFoveatedRenderingPropertiesVARJO*>(
					chain.Find<XrSystemFoveatedRenderingPropertiesVARJO>(type)))
				output->supportsFoveatedRendering = XR_TRUE;
			break;
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
