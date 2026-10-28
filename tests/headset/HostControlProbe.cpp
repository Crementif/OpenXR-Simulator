#include "Pch.h"
#include "headset/HostControl.h"

#include <iostream>

int main()
{
	try
	{
		simxr::headset::ValidateHostControlPipe();
		if (simxr::headset::HostStopRequested())
			return 2;
		std::cout << "ready" << std::endl;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!simxr::headset::HostStopRequested())
		{
			if (std::chrono::steady_clock::now() >= deadline)
				return 3;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
