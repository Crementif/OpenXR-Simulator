#pragma once

#include "app/CompanionSettings.h"
#include "app/LaunchTypes.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace simxr::app
{
// The launch profile editor: profiles on the left, the selected one's form on
// the right. Valid edits are stored as they are made.
class LaunchProfilesDialog
{
  public:
	LaunchProfilesDialog();
	~LaunchProfilesDialog();
	// Loads a stored profile into the editor; -1 starts a new one.
	void Edit(const CompanionSettings& settings, int index);
	// Stores a new profile made from an existing launch (a recent app) under a
	// unique name and edits it. Returns true when settings changed.
	bool Create(CompanionSettings& settings, const LaunchProfile& launch);
	// The edited profile.
	LaunchProfile Profile() const;
	int Selected() const noexcept
	{
		return m_selected;
	}
	// Returns true when Test launch was pressed. Sets changed when settings must be saved.
	bool Draw(CompanionSettings& settings, const LaunchSnapshot& process, bool& open, bool& changed);

  private:
	struct Variable
	{
		std::array<char, 256> key{};
		std::array<char, 4096> value{};
	};
	enum class Browse
	{
		Executable,
		WorkingDirectory,
		Manifest
	};
	class FileRequest;
	void Load(const LaunchProfile& profile);
	std::string NameProblem(const CompanionSettings& settings) const;
	std::string EnvironmentProblem() const;
	// Stores the form when it is valid; returns true when settings changed.
	bool Store(CompanionSettings& settings);
	void DrawList(CompanionSettings& settings, bool& changed);
	bool DrawForm(CompanionSettings& settings, const LaunchSnapshot& process, bool& changed);
	bool PathField(const char* label, std::array<char, 2048>& buffer, PathKind kind, Browse browse, bool optional);
	int m_selected = -1;
	std::array<char, 128> m_name{};
	std::array<char, 2048> m_executable{}, m_workingDirectory{}, m_manifest{}, m_rig{};
	std::array<char, 256> m_headset{};
	std::array<char, 16384> m_arguments{};
	std::vector<Variable> m_environment;
	std::optional<Browse> m_browsing;
	std::unique_ptr<FileRequest> m_file;
};
} // namespace simxr::app
