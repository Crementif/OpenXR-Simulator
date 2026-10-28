#pragma once

namespace simxr::app
{
class DeviceControlPanel;
class RuntimeSession;
class AdvancedSteppingPage
{
  public:
	AdvancedSteppingPage(RuntimeSession& session, DeviceControlPanel& devices) : m_session(session), m_devices(devices)
	{
	}
	void Draw();

  private:
	RuntimeSession& m_session;
	DeviceControlPanel& m_devices;
	int m_rawLayer = 0, m_rawView = 0;
};
} // namespace simxr::app
