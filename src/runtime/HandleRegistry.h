#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace simxr
{
inline std::atomic<std::uintptr_t> s_nextHandle{1};

template<typename Handle>
Handle AllocateHandle()
{
	auto identity = s_nextHandle.load(std::memory_order_relaxed);
	for (;;)
	{
		if (identity == std::numeric_limits<std::uintptr_t>::max())
			throw std::overflow_error("Handle identities exhausted");
		if (s_nextHandle.compare_exchange_weak(identity, identity + 1, std::memory_order_relaxed))
			break;
	}
	if constexpr (std::is_pointer_v<Handle>)
		return reinterpret_cast<Handle>(identity);
	else
		return static_cast<Handle>(identity);
}

template<typename Handle, typename Object>
class HandleRef
{
public:
	HandleRef() = default;
	HandleRef(Handle handle, std::shared_ptr<Object> object) : m_handle(handle), m_object(std::move(object))
	{
	}

	Object* operator->() const
	{
		return m_object.get();
	}

	operator Handle() const
	{
		return m_handle;
	}

	explicit operator bool() const
	{
		return static_cast<bool>(m_object);
	}

	bool operator==(const HandleRef& other) const
	{
		return m_handle == other.m_handle;
	}

	std::shared_ptr<Object> Shared() const
	{
		return m_object;
	}

private:
	Handle m_handle{};
	std::shared_ptr<Object> m_object;
};

template<typename Handle, typename Object, typename Parent>
class HandleRegistry
{
public:
	HandleRef<Handle, Object> Insert(Parent parent, std::unique_ptr<Object> object)
	{
		if (!object)
			throw std::invalid_argument("Missing handle object");
		return Create(parent, [&](Handle) { return std::move(object); });
	}

	template<typename Factory>
	HandleRef<Handle, Object> Create(Parent parent, Factory factory)
	{
		auto handle = AllocateHandle<Handle>();
		std::shared_ptr<Object> retained(factory(handle));
		if (!retained)
			throw std::invalid_argument("Missing handle object");
		std::lock_guard lock(m_mutex);
		m_objects.emplace(handle, Record{parent, retained});
		return {handle, std::move(retained)};
	}

	HandleRef<Handle, Object> Find(Handle handle) const
	{
		std::lock_guard lock(m_mutex);
		auto it = m_objects.find(handle);
		return it == m_objects.end() ? HandleRef<Handle, Object>{} : HandleRef<Handle, Object>{handle, it->second.object};
	}

	HandleRef<Handle, Object> Find(Handle handle, Parent parent) const
	{
		std::lock_guard lock(m_mutex);
		auto it = m_objects.find(handle);
		if (it == m_objects.end() || it->second.parent != parent)
			return {};
		return {handle, it->second.object};
	}

	bool Erase(Handle handle)
	{
		std::shared_ptr<Object> retired;
		{
			std::lock_guard lock(m_mutex);
			auto it = m_objects.find(handle);
			if (it == m_objects.end())
				return false;
			retired = std::move(it->second.object);
			m_objects.erase(it);
		}
		return true;
	}

	std::vector<Handle> Children(Parent parent) const
	{
		std::lock_guard lock(m_mutex);
		std::vector<Handle> children;
		for (const auto& [handle, record] : m_objects)
			if (record.parent == parent)
				children.push_back(handle);
		return children;
	}

private:
	struct Record
	{
		Parent parent;
		std::shared_ptr<Object> object;
	};
	mutable std::mutex m_mutex;
	std::unordered_map<Handle, Record> m_objects;
};
}
