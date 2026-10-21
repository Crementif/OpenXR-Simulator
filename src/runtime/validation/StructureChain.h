#pragma once

#include <array>
#include <cstddef>
#include <openxr/openxr.h>
#include <span>

namespace simxr
{
class StructureChain
{
public:
	explicit StructureChain(const void* first) : m_first(static_cast<const XrBaseInStructure*>(first))
	{
	}

	XrResult Validate(std::span<const XrStructureType> knownTypes) const
	{
		std::array<const XrBaseInStructure*, 128> visited{};
		std::size_t count = 0;
		for (auto node = m_first; node; node = node->next)
		{
			if (count == visited.size())
				return XR_ERROR_VALIDATION_FAILURE;
			for (std::size_t previous = 0; previous < count; ++previous)
			{
				if (visited[previous] == node)
					return XR_ERROR_VALIDATION_FAILURE;
				if (visited[previous]->type == node->type)
					for (auto type : knownTypes)
						if (node->type == type)
							return XR_ERROR_VALIDATION_FAILURE;
			}
			visited[count++] = node;
		}
		return XR_SUCCESS;
	}

	template<typename Structure>
	const Structure* Find(XrStructureType type) const
	{
		auto node = m_first;
		for (std::size_t count = 0; node && count < 128; ++count, node = node->next)
			if (node->type == type)
				return reinterpret_cast<const Structure*>(node);
		return nullptr;
	}

private:
	const XrBaseInStructure* m_first;
};
}
