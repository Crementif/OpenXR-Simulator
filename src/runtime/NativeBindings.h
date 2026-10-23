#pragma once

#include "runtime/HandleRegistry.h"

#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace simxr
{
template <typename Handle, typename Core, typename Native> class NativeBindings
{
  public:
	using CoreRef = HandleRef<Handle, Core>;
	using NativeRef = HandleRef<Handle, Native>;

	~NativeBindings()
	{
		// Native cleanup can call back into this table. Publish an empty table
		// before destroying bindings, while the mutex and table are still alive.
		decltype(m_bindings) retired;
		{
			std::lock_guard lock(m_mutex);
			retired = std::move(m_bindings);
		}
	}

	NativeRef Attach(CoreRef core, std::unique_ptr<Native> native)
	{
		if (!core || !native)
			throw std::invalid_argument("Missing native binding owner");
		const Handle handle = core;
		auto retained = std::make_shared<Binding>(Binding{std::move(core), std::move(native)});
		std::lock_guard lock(m_mutex);
		if (!m_bindings)
			m_bindings = std::make_unique<Bindings>();
		if (!m_bindings->emplace(handle, retained).second)
			throw std::logic_error("Duplicate native binding");
		return {handle, std::shared_ptr<Native>(retained, retained->native.get())};
	}

	NativeRef Find(CoreRef core) const
	{
		if (!core)
			return {};
		const Handle handle = core;
		std::lock_guard lock(m_mutex);
		if (!m_bindings)
			return {};
		const auto found = m_bindings->find(handle);
		if (found == m_bindings->end() || found->second->core.Shared() != core.Shared())
			return {};
		const auto& retained = found->second;
		return {handle, std::shared_ptr<Native>(retained, retained->native.get())};
	}

	bool Remove(Handle handle)
	{
		std::shared_ptr<Binding> retired;
		{
			std::lock_guard lock(m_mutex);
			if (!m_bindings)
				return false;
			const auto found = m_bindings->find(handle);
			if (found == m_bindings->end())
				return false;
			retired = std::move(found->second);
			m_bindings->erase(found);
		}
		return true;
	}

  private:
	struct Binding
	{
		CoreRef core;
		std::unique_ptr<Native> native;
	};
	mutable std::mutex m_mutex;
	using Bindings = std::unordered_map<Handle, std::shared_ptr<Binding>>;
	std::unique_ptr<Bindings> m_bindings;
};
} // namespace simxr
