#pragma once

#include "app/CompanionSettings.h"
#include "app/LaunchTypes.h"

#include <array>
#include <string>

namespace simxr::app
{
// The Launch profiles modal and the profile being edited in it.
class LaunchProfilesDialog
{
  public:
	// Loads a stored profile into the editor; -1 starts a new one.
	void Edit(const CompanionSettings& settings, int index);
	// The edited profile; throws when the environment text is malformed.
	LaunchProfile Profile() const;
	int Selected() const noexcept
	{
		return m_selected;
	}
	// Returns true when Launch was pressed. Sets changed when settings must be saved.
	bool Draw(CompanionSettings& settings, const LaunchSnapshot& process, bool& open, bool& changed,
			  std::string& error);

  private:
	void Store(CompanionSettings& settings);
	int m_selected = -1;
	std::array<char, 128> m_name{};
	std::array<char, 2048> m_executable{}, m_workingDirectory{}, m_manifest{}, m_rig{};
	std::array<char, 256> m_headset{};
	std::array<char, 16384> m_arguments{}, m_environment{};
};
} // namespace simxr::app
