#include "runtime/Instance.h"
#include "Pch.h"

#include "runtime/Session.h"

namespace simxr
{
Instance::Instance(XrInstance handle, std::span<const char* const> extensions, XrVersion apiVersion)
	: m_handle(handle), m_apiVersion(apiVersion), m_extensions(extensions, apiVersion), m_actions(handle, m_extensions),
	  m_trackers(m_actions, m_extensions, m_events)
{
	if (handle == XR_NULL_HANDLE)
		throw std::invalid_argument("Instance requires a handle");
}

XrInstance Instance::Handle() const
{
	return m_handle;
}

XrVersion Instance::ApiVersion() const
{
	return m_apiVersion;
}

const EnabledExtensions& Instance::Extensions() const
{
	return m_extensions;
}

EventQueue& Instance::Events()
{
	return m_events;
}

ActionRegistry& Instance::Actions()
{
	return m_actions;
}

TrackerInteraction& Instance::Trackers()
{
	return m_trackers;
}

std::unique_ptr<Session> Instance::CreateSession(XrSession handle, std::shared_ptr<Instance> owner,
												 std::shared_ptr<Clock> clock)
{
	if (owner.get() != this || !clock)
		throw std::invalid_argument("Invalid session owner or clock");
	return std::make_unique<Session>(handle, std::move(owner), std::move(clock));
}
} // namespace simxr
