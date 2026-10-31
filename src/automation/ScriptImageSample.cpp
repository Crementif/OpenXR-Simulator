#include "Pch.h"
#include "automation/ScriptEngine.h"
#include "automation/RigRuntime.h"
#include "automation/ScriptValues.h"
#include "control/ImageSample.h"

#include <lua.h>

namespace simxr::automation
{
using namespace script;

// Optional {rectangles = {{x, y, width, height}, {...}}} selects one source region per eye.
static std::optional<std::array<XrRect2Di, 2>> SampleRectangles(lua_State* state, int index)
{
	if (lua_isnoneornil(state, index))
		return {};
	if (!lua_istable(state, index))
		throw std::invalid_argument("Sample options require a table");
	lua_getfield(state, index, "rectangles");
	if (lua_isnil(state, -1))
	{
		lua_pop(state, 1);
		return {};
	}
	if (!lua_istable(state, -1) || lua_objlen(state, -1) != 2)
		throw std::invalid_argument("Sample rectangles require one table per eye");
	std::array<XrRect2Di, 2> rectangles{};
	for (int eye = 0; eye < 2; ++eye)
	{
		lua_rawgeti(state, -1, eye + 1);
		if (!lua_istable(state, -1))
			throw std::invalid_argument("Invalid sample rectangle");
		const auto rectangle = lua_absindex(state, -1);
		const auto coordinate = [&](const char* name) {
			const auto value = IntegerField(state, rectangle, name, 0);
			if (value > INT32_MAX)
				throw std::invalid_argument("Sample rectangle exceeds native coordinates");
			return static_cast<int>(value);
		};
		rectangles[eye] = {{coordinate("x"), coordinate("y")}, {coordinate("width"), coordinate("height")}};
		lua_pop(state, 1);
	}
	lua_pop(state, 1);
	return rectangles;
}

// Pixel geometry is small, so scripts get plain numbers for arithmetic rather than identifier strings.
static void PushNumberField(lua_State* state, const char* name, double value)
{
	lua_pushnumber(state, value);
	lua_setfield(state, -2, name);
}

static void PushImageSample(lua_State* state, const control::ImageSample& sample)
{
	lua_newtable(state);
	auto* pixels = lua_newbuffer(state, sample.pixels.size());
	std::memcpy(pixels, sample.pixels.data(), sample.pixels.size());
	lua_setfield(state, -2, "pixels");
	PushNumberField(state, "width", control::ImageSample::Width);
	PushNumberField(state, "height", control::ImageSample::Height);
	lua_createtable(state, 2, 0);
	for (int eye = 0; eye < 2; ++eye)
	{
		lua_newtable(state);
		const auto& rectangle = sample.rectangles[eye];
		PushNumberField(state, "x", rectangle[0]);
		PushNumberField(state, "y", rectangle[1]);
		PushNumberField(state, "width", rectangle[2]);
		PushNumberField(state, "height", rectangle[3]);
		lua_rawseti(state, -2, eye + 1);
	}
	lua_setfield(state, -2, "eyes");
}

int ScriptEngine::SampleHistoryImage(lua_State* state)
{
	const auto frame = Integer(state, 1);
	const auto target = HistoryTarget(state, 2);
	auto sample = m_runtime.SampleHistoryImage(frame, target, SampleRectangles(state, 2));
	if (!sample)
	{
		lua_pushnil(state);
		return 1;
	}
	PushImageSample(state, *sample);
	return 1;
}

int ScriptEngine::SampleWindowImage(lua_State* state)
{
	PushImageSample(state,
					m_runtime.SampleWindowImage(Integer(state, 1), Integer(state, 2), SampleRectangles(state, 3)));
	return 1;
}
}
