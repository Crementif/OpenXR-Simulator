#pragma once

#include <functional>
#include <string>

namespace simxr::app
{
struct Notice
{
	std::string title;
	std::string detail;
	std::string action;
	std::function<void()> run;
};
} // namespace simxr::app
