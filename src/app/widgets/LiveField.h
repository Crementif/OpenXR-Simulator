#pragma once
#include <functional>
#include <string>
#include <string_view>
namespace simxr::app
{
void DrawLiveField(bool pending, std::string_view rejection);
struct FieldValue
{
	std::function<void()> reset;
	bool modified = false;
	std::function<void(float)> nudge;
	std::string text;
	const char* group = nullptr;
	std::function<void()> resetGroup;
	std::string groupText;
	bool* quaternionView = nullptr;
};
bool FieldActions(const char* id, const FieldValue& value);
FieldValue NumberValue(float& value, float fallback, float step, float minimum, float maximum,
					   const char* format = "%.3f");
// An angle stored in radians, nudged and copied in degrees.
FieldValue AngleValue(float& radians, float fallback, float minimumDegrees, float maximumDegrees);
FieldValue SwitchValue(bool& value, bool fallback);
} // namespace simxr::app
