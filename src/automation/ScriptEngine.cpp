#include "automation/ScriptEngine.h"
#include "Pch.h"

#include "automation/RigProgress.h"
#include "automation/RigRuntime.h"
#include "automation/ScriptValues.h"

#include <cstdlib>
#include <limits>
#include <lua.h>
#include <luacode.h>
#include <lualib.h>
#include <stdexcept>

namespace simxr::automation
{
namespace wire = control;
using namespace script;
ScriptEngine::ScriptEngine(RigRuntime& runtime, RigProgress& progress, const RigOptions& options)
	: m_runtime(runtime), m_progress(progress), m_options(options)
{
}

ScriptEngine& ScriptEngine::Self(lua_State* state)
{
	return *static_cast<ScriptEngine*>(lua_callbacks(state)->userdata);
}
void* ScriptEngine::Allocate(void* context, void* pointer, std::size_t oldSize, std::size_t newSize)
{
	auto& engine = *static_cast<ScriptEngine*>(context);
	if (!pointer)
		oldSize = 0;
	if (!newSize)
	{
		std::free(pointer);
		engine.m_allocated -= oldSize;
		return nullptr;
	}
	if (newSize > oldSize && newSize - oldSize > engine.m_options.memoryLimit - engine.m_allocated)
		return nullptr;
	auto* allocation = std::realloc(pointer, newSize);
	if (allocation)
		engine.m_allocated = engine.m_allocated - oldSize + newSize;
	return allocation;
}
void ScriptEngine::Interrupt(lua_State* state, int gc)
{
	if (gc >= 0)
		return;
	auto& engine = Self(state);
	try
	{
		engine.m_progress.Check();
		return;
	}
	catch (const std::exception& error)
	{
		engine.m_bindingError = error.what();
	}
	luaL_error(state, "%s", engine.m_bindingError.c_str());
}
template <int (ScriptEngine::*Handler)(lua_State*)> int ScriptEngine::Binding(lua_State* state)
{
	auto& engine = Self(state);
	try
	{
		engine.m_progress.Check(true);
		return (engine.*Handler)(state);
	}
	catch (const std::exception& error)
	{
		engine.m_bindingError = error.what();
	}
	catch (...)
	{
		engine.m_bindingError = "Unknown rig binding failure";
	}
	luaL_error(state, "%s", engine.m_bindingError.c_str());
}
int ScriptEngine::FrameAdd(lua_State* state)
{
	const auto frame = Integer(state, 1);
	const auto offset = Integer(state, 2);
	if (offset > std::numeric_limits<std::uint64_t>::max() - frame)
		throw std::invalid_argument("Frame arithmetic exceeds uint64");
	PushInteger(state, frame + offset);
	return 1;
}
int ScriptEngine::FrameBefore(lua_State* state)
{
	lua_pushboolean(state, Integer(state, 1) < Integer(state, 2));
	return 1;
}
int ScriptEngine::Schedule(lua_State* state)
{
	if (!lua_istable(state, 1))
		throw std::invalid_argument("schedule requires a transaction table");
	CommandTarget target;
	const auto policyName = StringField(state, 1, "late_policy", "reject");
	CommandLatePolicy policy;
	if (policyName == "reject")
		policy = CommandLatePolicy::Reject;
	else if (policyName == "reschedule_next")
		policy = CommandLatePolicy::RescheduleNext;
	else
		throw std::invalid_argument("Invalid late policy");
	lua_getfield(state, 1, "frame");
	if (!lua_isnil(state, -1))
		target.frame = FrameId{Integer(state, -1)};
	lua_pop(state, 1);
	lua_getfield(state, 1, "time_ns");
	if (!lua_isnil(state, -1))
	{
		const auto time = Integer(state, -1);
		if (time > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
			throw std::invalid_argument("Simulation time exceeds int64");
		target.time = SimulationTime{Duration{static_cast<std::int64_t>(time)}};
	}
	lua_pop(state, 1);
	if (target.frame.has_value() == target.time.has_value())
		throw std::invalid_argument("Select frame or time_ns");
	lua_getfield(state, 1, "commands");
	if (!lua_istable(state, -1) || lua_objlen(state, -1) < 1 || lua_objlen(state, -1) > 64)
		throw std::invalid_argument("Expected 1 through 64 commands");
	std::vector<SimulationCommand> commands;
	for (int index = 1, count = lua_objlen(state, -1); index <= count; ++index)
	{
		lua_rawgeti(state, -1, index);
		commands.push_back(Command(state, -1));
		lua_pop(state, 1);
	}
	lua_pop(state, 1);
	PushInteger(state, m_runtime.Schedule(target, policy, std::move(commands)));
	return 1;
}
int ScriptEngine::AwaitTransaction(lua_State* state)
{
	PushInspection(state, m_runtime.AwaitTransaction(Integer(state, 1)));
	return 1;
}
int ScriptEngine::Pause(lua_State*)
{
	m_runtime.PauseApplication();
	return 0;
}
int ScriptEngine::Resume(lua_State*)
{
	m_runtime.ResumeApplication();
	return 0;
}
int ScriptEngine::Step(lua_State* state)
{
	ScriptCaptureTarget target;
	if (lua_istable(state, 1))
		target = CaptureTarget(state, 1);
	else if (!lua_isnoneornil(state, 1))
	{
		const auto layerIndex = Integer(state, 1);
		const auto viewIndex = lua_isnoneornil(state, 2) ? 0 : Integer(state, 2);
		if (layerIndex > std::numeric_limits<std::uint32_t>::max() ||
			viewIndex > std::numeric_limits<std::uint32_t>::max())
			throw std::invalid_argument("Capture index exceeds uint32");
		target = {wire::TargetKind::LayerView, static_cast<std::uint32_t>(layerIndex),
				  static_cast<std::uint32_t>(viewIndex)};
	}
	const auto step = m_runtime.StepApplication(target);
	if (step.capture)
		PushInteger(state, *step.capture);
	else
		lua_pushnil(state);
	lua_pushlstring(state, step.progress.data(), step.progress.size());
	return 2;
}
int ScriptEngine::StartInputRecording(lua_State* state)
{
	PushInspection(state, m_runtime.StartInputRecording());
	return 1;
}
int ScriptEngine::StopInputRecording(lua_State* state)
{
	PushInspection(state, m_runtime.StopInputRecording());
	return 1;
}
int ScriptEngine::ReadInputRecording(lua_State* state)
{
	const auto offset = lua_isnoneornil(state, 1) ? 0 : Integer(state, 1);
	const auto count = lua_isnoneornil(state, 2) ? 16 : Integer(state, 2);
	PushInspection(state, m_runtime.ReadInputRecording(offset, count));
	return 1;
}
int ScriptEngine::StartImageRecording(lua_State* state)
{
	ScriptCaptureTarget target;
	std::uint32_t capacity = 2;
	std::uint64_t frames = 0;
	if (!lua_isnoneornil(state, 1))
	{
		target = CaptureTarget(state, 1);
		capacity = IndexField(state, 1, "capacity", 2);
		frames = IntegerField(state, 1, "frames", 0);
	}
	m_runtime.StartImageRecording(target, capacity, frames);
	return 0;
}
int ScriptEngine::StopImageRecording(lua_State*)
{
	m_runtime.StopImageRecording();
	return 0;
}
int ScriptEngine::ImageRecording(lua_State* state)
{
	const auto recording = m_runtime.ImageRecording();
	lua_newtable(state);
	lua_pushlstring(state, recording.phase.data(), recording.phase.size());
	lua_setfield(state, -2, "phase");
	lua_pushboolean(state, recording.available);
	lua_setfield(state, -2, "available");
	PushIntegerField(state, "capacity", recording.capacity);
	PushIntegerField(state, "captured", recording.captured);
	PushIntegerField(state, "requested", recording.requested);
	if (recording.missedFrame)
		PushIntegerField(state, "missed_frame", *recording.missedFrame);
	lua_newtable(state);
	int index = 1;
	for (const auto capture : recording.captures)
	{
		PushInteger(state, capture);
		lua_rawseti(state, -2, index++);
	}
	lua_setfield(state, -2, "captures");
	return 1;
}
int ScriptEngine::Capture(lua_State* state)
{
	const auto capture = m_runtime.Capture(Integer(state, 1));
	lua_newtable(state);
	PushIntegerField(state, "capture_id", capture.identity);
	lua_pushlstring(state, capture.phase.data(), capture.phase.size());
	lua_setfield(state, -2, "phase");
	lua_pushboolean(state, capture.imageAvailable);
	lua_setfield(state, -2, "image_available");
	if (capture.frame)
		PushIntegerField(state, "frame_id", *capture.frame);
	return 1;
}
int ScriptEngine::ImageHistory(lua_State* state)
{
	const auto count = lua_isnoneornil(state, 1) ? 10 : Integer(state, 1);
	if (!count || count > 10)
		throw std::invalid_argument("Image history count must be in [1,10]");
	PushInspection(state, m_runtime.ImageHistory(static_cast<std::uint32_t>(count), HistoryTarget(state, 2)));
	return 1;
}
int ScriptEngine::ExportHistory(lua_State* state)
{
	m_runtime.ExportHistory(Integer(state, 1), String(state, 2), HistorySlice(state, 3), HistoryTarget(state, 4));
	return 0;
}
int ScriptEngine::BeginImageWindow(lua_State* state)
{
	const auto window = m_runtime.BeginImageWindow(HistoryTarget(state, 1));
	PushInspection(state, window.document);
	PushInteger(state, window.identity);
	return 2;
}
int ScriptEngine::ImageWindow(lua_State* state)
{
	PushInspection(state, m_runtime.ImageWindow(Integer(state, 1)));
	return 1;
}
int ScriptEngine::ExportWindowImage(lua_State* state)
{
	m_runtime.ExportWindowImage(Integer(state, 1), Integer(state, 2), String(state, 3), HistorySlice(state, 4));
	return 0;
}
int ScriptEngine::ReleaseImageWindow(lua_State* state)
{
	m_runtime.ReleaseImageWindow(Integer(state, 1));
	return 0;
}
int ScriptEngine::ProjectionHistory(lua_State* state)
{
	const auto count = lua_isnoneornil(state, 1) ? 64 : Integer(state, 1);
	if (!count || count > 64)
		throw std::invalid_argument("Projection history count must be in [1,64]");
	PushInspection(state, m_runtime.ProjectionHistory(static_cast<std::uint32_t>(count)));
	return 1;
}
int ScriptEngine::Inspect(lua_State* state)
{
	const auto inspection = m_runtime.Inspect(Integer(state, 1));
	PushInspection(state, inspection.document);
	PushInteger(state, inspection.identity);
	return 2;
}
int ScriptEngine::Revision(lua_State* state)
{
	PushInspection(state, m_runtime.Revision(Integer(state, 1)));
	return 1;
}
int ScriptEngine::ReleaseInspection(lua_State* state)
{
	m_runtime.ReleaseInspection(Integer(state, 1));
	return 0;
}
int ScriptEngine::StepDisplay(lua_State* state)
{
	const auto step = m_runtime.StepDisplay();
	if (step.capture)
		PushInteger(state, *step.capture);
	else
		lua_pushnil(state);
	lua_pushlstring(state, step.progress.data(), step.progress.size());
	if (step.frame)
		PushInteger(state, step.frame->value);
	else
		lua_pushnil(state);
	return 3;
}
int ScriptEngine::State(lua_State* state)
{
	PushInspection(state, m_runtime.State());
	return 1;
}
int ScriptEngine::ReleaseInputs(lua_State*)
{
	m_runtime.ReleaseInputs();
	return 0;
}
int ScriptEngine::Export(lua_State* state)
{
	std::optional<ScriptCaptureTarget> target;
	std::optional<std::uint32_t> arraySlice;
	if (!lua_isnoneornil(state, 3))
	{
		if (!lua_istable(state, 3))
			throw std::invalid_argument("Export options require a table");
		lua_getfield(state, 3, "target");
		const bool selectTarget = !lua_isnil(state, -1);
		lua_pop(state, 1);
		if (selectTarget)
			target = CaptureTarget(state, 3);
		lua_getfield(state, 3, "array_slice");
		const bool selectSlice = !lua_isnil(state, -1);
		lua_pop(state, 1);
		if (selectSlice)
			arraySlice = IndexField(state, 3, "array_slice", 0);
	}
	m_runtime.Export(Integer(state, 1), String(state, 2), target, arraySlice);
	return 0;
}
int ScriptEngine::ReleaseCapture(lua_State* state)
{
	m_runtime.ReleaseCapture(Integer(state, 1));
	return 0;
}
int ScriptEngine::Checkpoint(lua_State* state)
{
	RigCheckpoint checkpoint;
	checkpoint.name = String(state, 1);
	checkpoint.passed = lua_type(state, 2) == LUA_TBOOLEAN && lua_toboolean(state, 2);
	checkpoint.detail = lua_isnoneornil(state, 3) ? std::string{} : String(state, 3);
	m_progress.Checkpoint(std::move(checkpoint));
	return 0;
}
int ScriptEngine::Print(lua_State* state)
{
	std::string line;
	for (int index = 1, count = lua_gettop(state); index <= count; ++index)
	{
		if (index != 1)
			line += '\t';
		if (lua_isstring(state, index))
		{
			std::size_t length;
			const auto* value = lua_tolstring(state, index, &length);
			line.append(value, length);
		}
		else
			line += lua_typename(state, lua_type(state, index));
		if (line.size() > 4096)
			throw std::length_error("Rig output line exceeds 4096 bytes");
	}
	m_progress.Print(std::move(line));
	return 0;
}
int ScriptEngine::GameCommand(lua_State* state)
{
	const auto kind = Integer(state, 1);
	if (kind < 1 || kind > 3)
		throw std::invalid_argument("Game command kind must be reset=1, step=2 or custom=3");
	const auto payload = lua_isnoneornil(state, 2) ? std::string{} : String(state, 2);
	const auto command = m_runtime.GameCommand(static_cast<std::uint32_t>(kind), payload);
	PushInteger(state, command.operation);
	PushInteger(state, command.game);
	return 2;
}
int ScriptEngine::AwaitGame(lua_State* state)
{
	PushInspection(state, m_runtime.AwaitGame(Integer(state, 1), Integer(state, 2)));
	return 1;
}
int ScriptEngine::GameState(lua_State* state)
{
	PushInspection(state, m_runtime.GameState());
	return 1;
}

void ScriptEngine::Execute()
{
	std::unique_ptr<lua_State, decltype(&lua_close)> vm{lua_newstate(Allocate, this), lua_close};
	auto* state = vm.get();
	if (!state)
		throw std::bad_alloc();
	lua_callbacks(state)->userdata = this;
	lua_callbacks(state)->interrupt = Interrupt;
	luaL_openlibs(state);
	lua_pushcfunction(state, Binding<&ScriptEngine::Print>, "print");
	lua_setglobal(state, "print");
	lua_newtable(state);
	const auto bind = [&](const char* name, lua_CFunction function)
	{
		lua_pushcfunction(state, function, name);
		lua_setfield(state, -2, name);
	};
	bind("frame_add", Binding<&ScriptEngine::FrameAdd>);
	bind("frame_before", Binding<&ScriptEngine::FrameBefore>);
	bind("step_display", Binding<&ScriptEngine::StepDisplay>);
	bind("schedule", Binding<&ScriptEngine::Schedule>);
	bind("await_transaction", Binding<&ScriptEngine::AwaitTransaction>);
	bind("pause", Binding<&ScriptEngine::Pause>);
	bind("resume", Binding<&ScriptEngine::Resume>);
	bind("step", Binding<&ScriptEngine::Step>);
	bind("start_input_recording", Binding<&ScriptEngine::StartInputRecording>);
	bind("stop_input_recording", Binding<&ScriptEngine::StopInputRecording>);
	bind("read_input_recording", Binding<&ScriptEngine::ReadInputRecording>);
	bind("start_image_recording", Binding<&ScriptEngine::StartImageRecording>);
	bind("stop_image_recording", Binding<&ScriptEngine::StopImageRecording>);
	bind("image_recording", Binding<&ScriptEngine::ImageRecording>);
	bind("capture", Binding<&ScriptEngine::Capture>);
	bind("inspect", Binding<&ScriptEngine::Inspect>);
	bind("projection_history", Binding<&ScriptEngine::ProjectionHistory>);
	bind("image_history", Binding<&ScriptEngine::ImageHistory>);
	bind("export_history", Binding<&ScriptEngine::ExportHistory>);
	bind("begin_image_window", Binding<&ScriptEngine::BeginImageWindow>);
	bind("image_window", Binding<&ScriptEngine::ImageWindow>);
	bind("export_window_image", Binding<&ScriptEngine::ExportWindowImage>);
	bind("release_image_window", Binding<&ScriptEngine::ReleaseImageWindow>);
	bind("revision", Binding<&ScriptEngine::Revision>);
	bind("release_inspection", Binding<&ScriptEngine::ReleaseInspection>);
	bind("checkpoint", Binding<&ScriptEngine::Checkpoint>);
	bind("state", Binding<&ScriptEngine::State>);
	bind("release_inputs", Binding<&ScriptEngine::ReleaseInputs>);
	bind("export", Binding<&ScriptEngine::Export>);
	bind("release_capture", Binding<&ScriptEngine::ReleaseCapture>);
	bind("game_command", Binding<&ScriptEngine::GameCommand>);
	bind("await_game", Binding<&ScriptEngine::AwaitGame>);
	bind("game_state", Binding<&ScriptEngine::GameState>);
	PushInteger(state, m_options.seed);
	lua_setfield(state, -2, "seed");
	lua_setglobal(state, "simxr");
	lua_getglobal(state, "math");
	lua_getfield(state, -1, "randomseed");
	lua_pushnumber(state, static_cast<double>(m_options.seed & 0xffffffffULL));
	if (lua_pcall(state, 1, 0, 0) != LUA_OK)
		throw std::runtime_error("Cannot seed rig random generator");
	lua_pop(state, 1);
	luaL_sandbox(state);
	std::size_t bytecodeSize = 0;
	std::unique_ptr<char, decltype(&std::free)> bytecode{
		luau_compile(m_options.source.data(), m_options.source.size(), nullptr, &bytecodeSize), std::free};
	if (!bytecode)
		throw std::bad_alloc();
	const auto loaded = luau_load(state, m_options.name.c_str(), bytecode.get(), bytecodeSize, 0);
	bytecode.reset();
	if (loaded != LUA_OK || lua_pcall(state, 0, 0, 0) != LUA_OK)
		throw std::runtime_error(lua_tostring(state, -1) ? lua_tostring(state, -1) : "Luau execution failed");
	m_progress.Check();
}
} // namespace simxr::automation
