#include "Pch.h"
#include "automation/ScriptEngine.h"
#include "automation/RigProgress.h"
#include "automation/RigRuntime.h"
#include "automation/ScriptValues.h"
#include "control/IncidentSample.h"
#include "diagnostics/IncidentAnalysis.h"

#include <lua.h>

namespace simxr::automation
{
using namespace script;

int ScriptEngine::AnalyzeImageHistory(lua_State* state)
{
	const auto count = Integer(state, 1);
	if (!count || count > 10)
		throw std::invalid_argument("Analysis history count must be in [1,10]");
	m_progress.BeginObservation();
	PushInspection(state, m_runtime.ImageHistory(static_cast<std::uint32_t>(count), HistoryTarget(state, 2)));
	const auto result = AnalyzeImages(state, 0);
	m_progress.EndObservation();
	return result;
}

int ScriptEngine::AnalyzeImageWindow(lua_State* state)
{
	const auto window = Integer(state, 1);
	PushInspection(state, m_runtime.ImageWindow(window));
	if (StringField(state, lua_absindex(state, -1), "phase") != "complete")
		throw std::runtime_error("Image window must be complete before analysis");
	return AnalyzeImages(state, window);
}

int ScriptEngine::AnalyzeImages(lua_State* state, std::uint64_t window)
{
	const auto document = lua_absindex(state, -1);
	const auto target = StringField(state, document, "target");
	if (target != "quad_layers" && target != "composed_eyes")
		throw std::runtime_error("Unsupported analysis target");
	const bool ui = target == "quad_layers";
	flicker::IncidentAnalysis analysis(ui);
	lua_getfield(state, document, "entries");
	if (!lua_istable(state, -1) || lua_objlen(state, -1) > 22)
		throw std::runtime_error("Invalid incident window entries");
	const auto entries = lua_absindex(state, -1);

	struct Event
	{
		std::uint64_t frame;
		flicker::AnalysisObservation observation;
	};

	std::vector<Event> events;
	std::vector<MonitorFrame> frames;
	const auto count = lua_objlen(state, entries);
	for (int index = 1; index <= count; ++index)
	{
		m_progress.Check();
		lua_rawgeti(state, entries, index);
		if (!lua_istable(state, -1))
			throw std::runtime_error("Invalid incident frame");
		const auto entry = lua_absindex(state, -1);
		const auto frame = IntegerField(state, entry, "frame_id", 0);
		if (!frame)
			throw std::runtime_error("Incident frame identity is missing");
		const auto phase = StringField(state, entry, "phase");
		if (phase != "ready" && phase != "clear")
		{
			analysis.Skip(frame);
			events.push_back({frame, {true, "image_unavailable"}});
			frames.push_back({frame, true, "image_unavailable"});
		}
		else
		{
			control::IncidentSample sample;
			if (phase == "clear")
				sample.pixels.resize(std::size_t(sample.Width) * sample.Height * 4);
			else
			{
				std::optional<std::array<XrRect2Di, 2>> rectangles;
				if (ui)
				{
					rectangles.emplace();
					lua_getfield(state, entry, "quad_rectangles");
					if (!lua_istable(state, -1) || lua_objlen(state, -1) != 2)
						throw std::runtime_error("Invalid incident quad rectangles");
					for (unsigned eye = 0; eye < 2; ++eye)
					{
						lua_rawgeti(state, -1, eye + 1);
						if (!lua_istable(state, -1))
							throw std::runtime_error("Invalid incident quad rectangle");
						const auto coordinate = [&](const char* name) {
							const auto value = IntegerField(state, lua_absindex(state, -1), name, 0);
							if (value > INT32_MAX)
								throw std::runtime_error("Incident rectangle exceeds native coordinates");
							return static_cast<int>(value);
						};
						(*rectangles)[eye] = {{coordinate("x"), coordinate("y")},
											  {coordinate("width"), coordinate("height")}};
						lua_pop(state, 1);
					}
					lua_pop(state, 1);
				}
				if (rectangles && (!(*rectangles)[0].extent.width || !(*rectangles)[0].extent.height ||
								   !(*rectangles)[1].extent.width || !(*rectangles)[1].extent.height))
				{
					analysis.Skip(frame);
					events.push_back({frame, {true, "missing_eye"}});
					frames.push_back({frame, true, "missing_eye"});
					lua_pop(state, 1);
					continue;
				}
				if (window)
					sample = m_runtime.SampleWindowImage(window, frame, rectangles);
				else
				{
					auto observed = m_runtime.SampleHistoryImage(
						frame, ui ? control::TargetKind::QuadLayers : control::TargetKind::ComposedEyes, rectangles);
					if (!observed)
					{
						analysis.Skip(frame);
						events.push_back({frame, {true, "evicted"}});
						frames.push_back({frame, true, "evicted"});
						lua_pop(state, 1);
						continue;
					}
					sample = std::move(*observed);
				}
			}
			auto observation = analysis.Observe(frame, sample.pixels, sample.Width, sample.Height, sample.Width * 4,
												flicker::PixelOrder::Rgba, sample.rectangles);
			frames.push_back({frame, observation.skipped, observation.reason});
			if (!observation.reason.empty())
				events.push_back({frame, std::move(observation)});
		}
		lua_pop(state, 1);
	}
	lua_pop(state, 2);
	lua_newtable(state);
	const auto& counts = analysis.Counts();
	if (!frames.empty())
		m_progress.ObserveWindow(frames, counts.observations, counts.skipped, counts.temporalPairs, counts.gaps);
	for (const auto& [name, value] :
		 {std::pair{"frames_analyzed", counts.observations}, std::pair{"frames_skipped", counts.skipped},
		  std::pair{"temporal_pairs", counts.temporalPairs}, std::pair{"frame_gaps", counts.gaps}})
	{
		lua_pushnumber(state, static_cast<double>(value));
		lua_setfield(state, -2, name);
	}
	bool detected = false;
	for (const bool skipped : {false, true})
	{
		lua_newtable(state);
		unsigned index = 0;
		for (const auto& event : events)
		{
			if (event.observation.skipped != skipped)
				continue;
			detected |= !skipped;
			lua_newtable(state);
			PushIntegerField(state, "frame_id", event.frame);
			lua_pushlstring(state, event.observation.reason.data(), event.observation.reason.size());
			lua_setfield(state, -2, "reason");
			lua_rawseti(state, -2, ++index);
		}
		lua_setfield(state, -2, skipped ? "skipped_frames" : "events");
	}
	lua_pushboolean(state, detected);
	lua_setfield(state, -2, "detected");
	return 1;
}
}
