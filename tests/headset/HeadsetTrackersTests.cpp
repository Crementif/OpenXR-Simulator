#include "../TestSupport.h"
#include "Pch.h"
#include "headset/HeadsetTrackers.h"
#include "headset/OpenXrRuntime.h"

#include <iostream>

namespace simxr::headset::test
{
static std::vector<std::string> s_paths{1};
static std::vector<XrViveTrackerPathsHTCX> s_devices;
static std::vector<XrExtensionProperties> s_extensions;
static std::size_t s_actions = 0, s_destroyed = 0;
static std::vector<XrPath> s_observedPaths;

static XrPath Path(std::string text)
{
	const auto found = std::find(s_paths.begin(), s_paths.end(), text);
	if (found != s_paths.end())
		return found - s_paths.begin();
	s_paths.push_back(std::move(text));
	return s_paths.size() - 1;
}

static XrResult XRAPI_CALL Enumerate(XrInstance, std::uint32_t capacity, std::uint32_t* count,
									 XrViveTrackerPathsHTCX* paths)
{
	*count = s_devices.size();
	if (capacity < s_devices.size())
		return XR_ERROR_SIZE_INSUFFICIENT;
	std::copy(s_devices.begin(), s_devices.end(), paths);
	return XR_SUCCESS;
}
} // namespace simxr::headset::test

using namespace simxr::headset::test;

namespace simxr::headset
{
OpenXrRuntime::~OpenXrRuntime() = default;

XrInstance OpenXrRuntime::Instance() const
{
	return reinterpret_cast<XrInstance>(1);
}

bool OpenXrRuntime::ResourcesPreserved() const
{
	return false;
}

const std::vector<XrExtensionProperties>& OpenXrRuntime::Extensions() const
{
	return s_extensions;
}

PFN_xrVoidFunction OpenXrRuntime::Procedure(const char*) const
{
	return reinterpret_cast<PFN_xrVoidFunction>(Enumerate);
}

void CheckXr(XrResult result, const char* operation)
{
	if (XR_FAILED(result))
		throw std::runtime_error(operation);
}
} // namespace simxr::headset

extern "C"
{
	XRAPI_ATTR XrResult XRAPI_CALL xrStringToPath(XrInstance, const char* text, XrPath* path)
	{
		*path = Path(text);
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL xrPathToString(XrInstance, XrPath path, std::uint32_t capacity,
												  std::uint32_t* length, char* text)
	{
		const auto& value = s_paths.at(path);
		*length = value.size() + 1;
		if (capacity < *length)
			return XR_ERROR_SIZE_INSUFFICIENT;
		std::strcpy(text, value.c_str());
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(XrActionSet, const XrActionCreateInfo* info, XrAction* action)
	{
		Require(info->actionType == XR_ACTION_TYPE_POSE_INPUT && info->countSubactionPaths == 1,
				"Tracker pose action contract");
		*action = reinterpret_cast<XrAction>(++s_actions);
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(XrSession, const XrActionSpaceCreateInfo* info, XrSpace* space)
	{
		*space = reinterpret_cast<XrSpace>(info->action);
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace)
	{
		++s_destroyed;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(XrInstance,
																	   const XrInteractionProfileSuggestedBinding* info)
	{
		Require(info->countSuggestedBindings == s_actions, "Every tracker role needs a binding");
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStatePose(XrSession, const XrActionStateGetInfo* info,
														XrActionStatePose* state)
	{
		s_observedPaths.push_back(info->subactionPath);
		state->isActive = true;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace, XrSpace, XrTime, XrSpaceLocation* location)
	{
		location->pose.orientation.w = 1;
		location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
		return XR_SUCCESS;
	}
}

int main()
{
	try
	{
		using namespace simxr;
		using namespace simxr::headset;
		OpenXrRuntime runtime;
		for (std::uint32_t revision : {0U, 1U, 3U})
		{
			s_actions = s_destroyed = 0;
			s_extensions.clear();
			if (revision)
			{
				XrExtensionProperties extension{XR_TYPE_EXTENSION_PROPERTIES};
				std::strcpy(extension.extensionName, XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME);
				extension.extensionVersion = revision;
				s_extensions.push_back(extension);
			}
			const auto waist = Path("/user/vive_tracker_htcx/role/waist");
			s_devices = {{XR_TYPE_VIVE_TRACKER_PATHS_HTCX, nullptr, Path("/device/b"), waist},
						 {XR_TYPE_VIVE_TRACKER_PATHS_HTCX, nullptr, Path("/device/a"), XR_NULL_PATH}};
			{
				HeadsetTrackers provider(runtime);
				provider.Open(reinterpret_cast<XrSession>(1), reinterpret_cast<XrActionSet>(1));
				Require(provider.Available() == (revision != 0), "Optional provider availability");
				Require(s_actions == (revision == 3	  ? 19
									  : revision == 1 ? 15
													  : 0),
						"Revision-specific role declaration");
				if (!revision)
					continue;
				auto sample = provider.Sample(1, reinterpret_cast<XrSpace>(99));
				Require(sample.size() == 2 && sample[0].persistentPath == "/device/a" &&
							sample[0].role == TrackerRole::Unassigned &&
							sample[0].location.locationFlags == XR_SPACE_LOCATION_ORIENTATION_VALID_BIT &&
							sample[1].role == TrackerRole::Waist &&
							sample[1].location.locationFlags == XR_SPACE_LOCATION_ORIENTATION_VALID_BIT,
						"Identity, unassigned role or partial validity lost");
				s_devices[1].rolePath = waist;
				sample = provider.Sample(2, reinterpret_cast<XrSpace>(99));
				Require(sample[0].location.locationFlags && sample[1].location.locationFlags &&
							s_observedPaths.back() == Path("/device/a"),
						"Persistent poses fell back to an ambiguous role");
				const auto declarations = s_actions;
				s_devices.push_back({XR_TYPE_VIVE_TRACKER_PATHS_HTCX, nullptr, Path("/device/late"), XR_NULL_PATH});
				const auto late = provider.Sample(2, reinterpret_cast<XrSpace>(99));
				Require(late.size() == 3 && late.back().needsBinding && !late.back().location.locationFlags &&
							s_actions == declarations,
						"Late unassigned tracker invented a pose or mutated attached actions");
				s_devices.back().rolePath = Path("/user/vive_tracker_htcx/role/chest");
				const auto assigned = provider.Sample(2, reinterpret_cast<XrSpace>(99));
				Require(assigned.back().location.locationFlags && !assigned.back().needsBinding &&
							s_observedPaths.back() == s_devices.back().rolePath && s_actions == declarations,
						"Late assigned tracker did not use its predeclared unique role");
				s_devices.pop_back();
				s_devices[1].persistentPath = s_devices[0].persistentPath;
				bool rejected = false;
				try
				{
					provider.Sample(3, reinterpret_cast<XrSpace>(99));
				}
				catch (const std::runtime_error&)
				{
					rejected = true;
				}
				Require(rejected, "Duplicate persistent paths accepted");
				s_devices.clear();
				for (std::size_t index = 0; index < MaxTrackerDevices; ++index)
					s_devices.push_back({XR_TYPE_VIVE_TRACKER_PATHS_HTCX, nullptr,
										 Path("/device/" + std::to_string(index)), XR_NULL_PATH});
				sample = provider.Sample(4, reinterpret_cast<XrSpace>(99));
				Require(sample.size() == MaxTrackerDevices, "Full tracker inventory was truncated");
				std::reverse(s_devices.begin(), s_devices.end());
				const auto reordered = provider.Sample(4, reinterpret_cast<XrSpace>(99));
				for (std::size_t index = 0; index < sample.size(); ++index)
					Require(sample[index].persistentPath == reordered[index].persistentPath,
							"Runtime enumeration order changed device identity order");
				s_devices.resize(65);
				rejected = false;
				try
				{
					provider.Sample(4, reinterpret_cast<XrSpace>(99));
				}
				catch (const std::runtime_error&)
				{
					rejected = true;
				}
				Require(rejected, "Excess trackers silently truncated");
				s_devices.clear();
				Require(provider.Sample(5, reinterpret_cast<XrSpace>(99)).empty(),
						"Disconnected inventory retained devices");
			}
			Require(s_destroyed == s_actions, "Tracker spaces leaked");
		}
		s_actions = s_destroyed = 0;
		s_devices.clear();
		for (std::size_t index = 0; index < MaxTrackerDevices; ++index)
			s_devices.push_back({XR_TYPE_VIVE_TRACKER_PATHS_HTCX, nullptr,
								 Path("/device/startup_" + std::to_string(index)), XR_NULL_PATH});
		{
			HeadsetTrackers provider(runtime);
			provider.Open(reinterpret_cast<XrSession>(1), reinterpret_cast<XrActionSet>(1));
			Require(s_actions == MaxTrackerDevices + 17, "Startup tracker actions were truncated");
			const auto full = provider.Sample(6, reinterpret_cast<XrSpace>(99));
			Require(full.size() == MaxTrackerDevices && std::all_of(full.begin(), full.end(),
																	[](const PhysicalTrackerSample& sample) {
																		return sample.location.locationFlags != 0;
																	}),
					"Unassigned startup trackers did not retain independent poses");
		}
		Require(s_destroyed == s_actions, "Persistent tracker spaces leaked");
		std::cout << "Physical tracker provider checks passed\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
