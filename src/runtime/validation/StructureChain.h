#pragma once

#include <cstddef>
#include <openxr/openxr.h>
#include <span>

namespace simxr
{
class StructureChain
{
public:
	explicit StructureChain(const void* first) : m_first(static_cast<const XrBaseInStructure*>(first)) {}

	XrResult Validate(std::span<const XrStructureType> knownTypes) const;

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
} // namespace simxr
