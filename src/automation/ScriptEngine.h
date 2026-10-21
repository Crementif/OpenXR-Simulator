#pragma once

#include <memory>

namespace simxr::automation
{
class RigRuntime;
class RigProgress;
struct RigOptions;

class ScriptEngine
{
  public:
	ScriptEngine(RigRuntime& runtime, RigProgress& progress, const RigOptions& options);
	~ScriptEngine();
	void Execute();

  private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace simxr::automation
