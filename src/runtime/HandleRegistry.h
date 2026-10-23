#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace simxr
{
std::uintptr_t AllocateHandleIdentity();

template <typename Handle> Handle AllocateHandle()
{
	const auto identity = AllocateHandleIdentity();
	if constexpr (std::is_pointer_v<Handle>)
		return reinterpret_cast<Handle>(identity);
	else
		return static_cast<Handle>(identity);
}

template <typename Handle, typename Object> class HandleRef
{
  public:
	HandleRef() = default;
	HandleRef(Handle handle, std::shared_ptr<Object> object) : m_handle(handle), m_object(std::move(object)) {}

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

template <typename Handle, typename Object, typename Parent> class HandleRegistry
{
	struct Record
	{
		Parent parent;
		std::shared_ptr<Object> object;
	};
	using Records = std::unordered_map<Handle, Record>;

  public:
	class Retired
	{
	  public:
		Retired() = default;
		Retired(Retired&&) = default;
		Retired& operator=(Retired&&) = default;

		template <typename Visitor> void ForEach(Visitor visitor) const
		{
			if (!m_objects)
				return;
			for (const auto& [handle, record] : *m_objects)
				visitor(HandleRef<Handle, Object>{handle, record.object});
		}

	  private:
		friend class HandleRegistry;
		std::unique_ptr<Records> m_objects;
	};

	~HandleRegistry()
	{
		// Publish an empty table before owned destructors can reenter it.
		auto retired = RetireAll();
	}

	Retired RetireAll()
	{
		Retired retired;
		{
			std::lock_guard lock(m_mutex);
			retired.m_objects = std::move(m_objects);
		}
		return retired;
	}

	HandleRef<Handle, Object> Insert(Parent parent, std::unique_ptr<Object> object)
	{
		if (!object)
			throw std::invalid_argument("Missing handle object");
		return Create(parent,
					  [&](Handle)
					  {
						  return std::move(object);
					  });
	}

	template <typename Factory> HandleRef<Handle, Object> Create(Parent parent, Factory factory)
	{
		const auto handle = AllocateHandle<Handle>();
		std::shared_ptr<Object> retained(factory(handle));
		if (!retained)
			throw std::invalid_argument("Missing handle object");
		std::lock_guard lock(m_mutex);
		if (!m_objects)
			m_objects = std::make_unique<Records>();
		m_objects->emplace(handle, Record{parent, retained});
		return {handle, std::move(retained)};
	}

	HandleRef<Handle, Object> Find(Handle handle) const
	{
		std::lock_guard lock(m_mutex);
		if (!m_objects)
			return {};
		const auto found = m_objects->find(handle);
		if (found == m_objects->end())
			return {};
		return {handle, found->second.object};
	}

	HandleRef<Handle, Object> Find(Handle handle, Parent parent) const
	{
		std::lock_guard lock(m_mutex);
		if (!m_objects)
			return {};
		const auto found = m_objects->find(handle);
		if (found == m_objects->end() || found->second.parent != parent)
			return {};
		return {handle, found->second.object};
	}

	bool Erase(Handle handle)
	{
		std::shared_ptr<Object> retired;
		{
			std::lock_guard lock(m_mutex);
			if (!m_objects)
				return false;
			const auto found = m_objects->find(handle);
			if (found == m_objects->end())
				return false;
			retired = std::move(found->second.object);
			m_objects->erase(found);
		}
		return true;
	}

	std::vector<Handle> Children(Parent parent) const
	{
		std::lock_guard lock(m_mutex);
		std::vector<Handle> children;
		if (m_objects)
			for (const auto& [handle, record] : *m_objects)
				if (record.parent == parent)
					children.push_back(handle);
		return children;
	}

  private:
	mutable std::mutex m_mutex;
	std::unique_ptr<Records> m_objects;
};
} // namespace simxr
