#pragma once

#include "runtime/EventQueue.h"
#include "runtime/ExtensionRegistry.h"
#include "simulation/ActionRegistry.h"
#include "runtime/extensions/TrackerInteraction.h"

#include <openxr/openxr.h>
#include <memory>
#include <span>

namespace simxr
{
class Session;
class Clock;

class Instance
{
public:
	Instance(XrInstance handle, std::span<const char* const> extensions, XrVersion apiVersion);
	XrInstance Handle() const;
	XrVersion ApiVersion() const;
	const EnabledExtensions& Extensions() const;
	EventQueue& Events();
	ActionRegistry& Actions();
	TrackerInteraction& Trackers();
	std::unique_ptr<Session> CreateSession(XrSession handle, std::shared_ptr<Instance> owner, std::shared_ptr<Clock> clock);

private:
	XrInstance m_handle;
	XrVersion m_apiVersion;
	const EnabledExtensions m_extensions;
	EventQueue m_events;
	ActionRegistry m_actions;
	TrackerInteraction m_trackers;
};
}
