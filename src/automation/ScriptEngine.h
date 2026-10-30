#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct lua_State;

namespace simxr::automation
{
class RigRuntime;
class RigProgress;
struct RigOptions;

class ScriptEngine
{
public:
	ScriptEngine(RigRuntime& runtime, RigProgress& progress, const RigOptions& options);
	ScriptEngine(const ScriptEngine&) = delete;
	ScriptEngine& operator=(const ScriptEngine&) = delete;

	void Execute();

private:
	static ScriptEngine& Self(lua_State* state);
	static void* Allocate(void* context, void* pointer, std::size_t oldSize, std::size_t newSize);
	static void Interrupt(lua_State* state, int gc);
	template<int (ScriptEngine::*Handler)(lua_State*)>
	static int Binding(lua_State* state);

	int Wait(lua_State* state);
	int MonotonicTime(lua_State* state);
	int FrameAdd(lua_State* state);
	int FrameBefore(lua_State* state);
	int Schedule(lua_State* state);
	int AwaitTransaction(lua_State* state);
	int Pause(lua_State* state);
	int Resume(lua_State* state);
	int Step(lua_State* state);
	int StartInputRecording(lua_State* state);
	int StopInputRecording(lua_State* state);
	int ReadInputRecording(lua_State* state);
	int StartImageRecording(lua_State* state);
	int StopImageRecording(lua_State* state);
	int ImageRecording(lua_State* state);
	int Capture(lua_State* state);
	int ImageHistory(lua_State* state);
	int ExportHistory(lua_State* state);
	int BeginImageWindow(lua_State* state);
	int ImageWindow(lua_State* state);
	int SampleWindowImage(lua_State* state);
	int SampleHistoryImage(lua_State* state);
	int ExportWindowImage(lua_State* state);
	int ReleaseImageWindow(lua_State* state);
	int ProjectionHistory(lua_State* state);
	int Inspect(lua_State* state);
	int Revision(lua_State* state);
	int ReleaseInspection(lua_State* state);
	int StepDisplay(lua_State* state);
	int AdvanceToFrame(lua_State* state);
	int State(lua_State* state);
	int ReleaseInputs(lua_State* state);
	int Export(lua_State* state);
	int ReleaseCapture(lua_State* state);
	int Checkpoint(lua_State* state);
	int Print(lua_State* state);
	int GameCommand(lua_State* state);
	int AwaitGame(lua_State* state);
	int GameState(lua_State* state);

	RigRuntime& m_runtime;
	RigProgress& m_progress;
	const RigOptions& m_options;
	std::size_t m_allocated = 0;
	std::string m_bindingError;
};
} // namespace simxr::automation
