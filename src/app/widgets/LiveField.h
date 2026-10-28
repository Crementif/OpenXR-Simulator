#pragma once
#include <functional>
#include <string>
#include <string_view>
namespace simxr::app
{
void DrawLiveField(bool pending, std::string_view rejection);
// What a field can do with the value behind the last item, besides the item's own editing.
struct FieldValue
{
	// Puts this value back to its default; modified marks the field while it differs.
	std::function<void()> reset;
	bool modified = false;
	// Adds steps of the value's increment; empty when the wheel cannot change it.
	std::function<void(float)> nudge;
	// The value as text for Copy.
	std::string text;
	// A wider reset and copy offered in the menu, such as the whole pose.
	const char* group = nullptr;
	std::function<void()> resetGroup;
	std::string groupText;
	// Rotation fields can change their display without authoring a new pose.
	bool* quaternionView = nullptr;
};
// Shift+right-click resets; wheel nudges (Shift x10, Ctrl x0.1); right-click opens Reset value and Copy,
// for the last item. Draws the modified mark. Returns true when the value changed.
bool FieldActions(const char* id, const FieldValue& value);
// A float field with its default and wheel step; the text uses format.
FieldValue NumberValue(float& value, float fallback, float step, float minimum, float maximum,
					   const char* format = "%.3f");
// An angle stored in radians, nudged and copied in degrees.
FieldValue AngleValue(float& radians, float fallback, float minimumDegrees, float maximumDegrees);
FieldValue SwitchValue(bool& value, bool fallback);
} // namespace simxr::app
