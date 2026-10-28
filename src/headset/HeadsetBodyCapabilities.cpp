#include "headset/HeadsetBodyCapabilities.h"
#include "Pch.h"
#include "headset/OpenXrRuntime.h"

namespace simxr::headset
{
HeadsetBodyCapabilities ReadHeadsetBodyCapabilities(const OpenXrRuntime& runtime)
{
	XrSystemBodyTrackingPropertiesFB fb{XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB};
	XrSystemPropertiesBodyTrackingFullBodyMETA meta{XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FULL_BODY_META};
	XrSystemBodyTrackingPropertiesBD bd{XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_BD};
	XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
	if (runtime.Supports(XR_FB_BODY_TRACKING_EXTENSION_NAME))
	{
		fb.next = properties.next;
		properties.next = &fb;
		if (runtime.Supports(XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME))
		{
			meta.next = properties.next;
			properties.next = &meta;
		}
	}
	if (runtime.Supports(XR_BD_BODY_TRACKING_EXTENSION_NAME))
	{
		bd.next = properties.next;
		properties.next = &bd;
	}
	if (properties.next)
		CheckXr(xrGetSystemProperties(runtime.Instance(), runtime.System(), &properties),
				"Read physical body tracking capabilities");
	return {fb.supportsBodyTracking == XR_TRUE,
			fb.supportsBodyTracking == XR_TRUE && meta.supportsFullBodyTracking == XR_TRUE,
			bd.supportsBodyTracking == XR_TRUE};
}
} // namespace simxr::headset
