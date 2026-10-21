#pragma once

#include "app/CompanionSettings.h"
#include "app/ShellState.h"

#include <memory>
#include <string>

namespace simxr::app
{
class RuntimePanels;
class ProcessSupervisor;
class WorkspacePanels
{
  public:
	// Disabled for finite QA/proof runs, including both reading and writing config.
	WorkspacePanels(ProcessSupervisor& processes, bool enabled, std::string endpoint);
	~WorkspacePanels();
	std::string InitialEndpoint() const;
	void BeginFrame(RuntimePanels& runtime);
	void Draw(RuntimePanels& runtime);
	const ShellState& State() const;
	void Select(Selection selection);
	void SetPreviewExpanded(bool expanded);
	void SetScale(float scale);
	void Finish();

  private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace simxr::app
