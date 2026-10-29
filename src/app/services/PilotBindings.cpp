#include "Pch.h"
#include "app/services/PilotBindings.h"

namespace simxr::app
{
std::string PilotBindingLabel(const PilotBinding& binding)
{
	return (binding.modifiers & 1 ? std::string{"Ctrl+"} : std::string{}) + (binding.modifiers & 2 ? "Shift+" : "") +
		   (binding.modifiers & 4 ? "Alt+" : "") + (binding.modifiers & 8 ? "Super+" : "") + binding.key;
}

PilotBindings DefaultPilotBindings()
{
	return {{{"MouseRight"}, {"W"},		   {"S"},		{"A"},		 {"D"},			{"Q"},		   {"E"},
			 {"LeftShift"},	 {"LeftCtrl"}, {"LeftAlt"}, {"Z"},		 {"X"},			{"MouseLeft"}, {"MouseMiddle"},
			 {"R"},			 {"T"},		   {"G"},		{"UpArrow"}, {"DownArrow"}, {"LeftArrow"}, {"RightArrow"},
			 {"C"},			 {"Tab"},	   {"1"},		{"2"},		 {"3"},			{"0"},		   {"Escape"}}};
}
}
