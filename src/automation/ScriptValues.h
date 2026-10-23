#pragma once

#include "automation/RigRuntime.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

struct lua_State;

namespace simxr::automation::script
{
std::uint64_t Integer(lua_State* state, int index);
std::string String(lua_State* state, int index);
std::uint64_t IntegerField(lua_State* state, int table, const char* key, std::uint64_t fallback);
std::uint32_t IndexField(lua_State* state, int table, const char* key, std::uint32_t fallback);
std::string StringField(lua_State* state, int table, const char* key, std::string fallback = {});
ScriptCaptureTarget CaptureTarget(lua_State* state, int index);
control::TargetKind HistoryTarget(lua_State* state, int index);
std::optional<std::uint32_t> HistorySlice(lua_State* state, int index);
SimulationCommand Command(lua_State* state, int index);
void PushInteger(lua_State* state, std::uint64_t value);
void PushIntegerField(lua_State* state, const char* name, std::uint64_t value);
void PushInspection(lua_State* state, std::span<const std::byte> bytes, unsigned depth = 0);
} // namespace simxr::automation::script
