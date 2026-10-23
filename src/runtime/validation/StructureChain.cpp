#include "runtime/validation/StructureChain.h"
#include "Pch.h"

namespace simxr
{
XrResult StructureChain::Validate(std::span<const XrStructureType> knownTypes) const
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
} // namespace simxr
