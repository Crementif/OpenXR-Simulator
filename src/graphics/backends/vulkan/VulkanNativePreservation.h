#pragma once

#include "graphics/backends/vulkan/VulkanDevice.h"

#ifdef _WIN32
#include "platform/windows/ModuleReference.h"
#endif

#include <array>
#include <cassert>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

namespace simxr
{
// Native safety storage, not a completion observer. Each resource reserves its
// final parking cell before acquisition. Unknown completion transfers the entire
// charged native owner here without allocation, waiting or destroying anything.
// Process lifetime is deliberate: shutdown is not proof of GPU completion.
template<class Resource, std::size_t Capacity>
class VulkanNativePreservation
{
	struct Storage
	{
		std::mutex mutex;
		std::array<bool, Capacity> reserved{};
		std::array<std::unique_ptr<Resource>, Capacity> preserved;

		std::size_t Reserve(const char* diagnostic)
		{
			std::lock_guard lock(mutex);
			for (std::size_t index = 0; index < Capacity; ++index)
				if (!reserved[index])
				{
					reserved[index] = true;
					return index;
				}
			throw VulkanError(VK_ERROR_OUT_OF_POOL_MEMORY, diagnostic);
		}

		void Release(std::size_t index) noexcept
		{
			std::lock_guard lock(mutex);
			reserved[index] = false;
		}

		void Preserve(std::size_t index, std::unique_ptr<Resource> resource) noexcept
		{
			std::lock_guard lock(mutex);
			assert(reserved[index] && !preserved[index]);
			preserved[index].swap(resource);
		}
	};

	static Storage& Owners()
	{
		static auto* s_owners = new Storage;
		return *s_owners;
	}

public:
	class Reservation
	{
	public:
		explicit Reservation(const char* diagnostic) : m_index(Owners().Reserve(diagnostic)) {}

		~Reservation()
		{
			Owners().Release(m_index);
		}

		Reservation(const Reservation&) = delete;
		Reservation& operator=(const Reservation&) = delete;

		void Preserve(std::unique_ptr<Resource> resource) noexcept
		{
			Owners().Preserve(m_index, std::move(resource));
		}

	private:
#ifdef _WIN32
		windows::ModuleReference m_module;
#endif
		const std::size_t m_index;
	};
};
} // namespace simxr
