#pragma once

#include <openxr/openxr.h>

namespace simxr::probe
{
XrResult CheckNativeAdmission(XrInstance instance, XrSession session, const XrFrameEndInfo& frame);
}
