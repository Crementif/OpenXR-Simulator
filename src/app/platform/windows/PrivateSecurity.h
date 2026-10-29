#pragma once

#include "app/platform/windows/LaunchSupport.h"

#include <aclapi.h>
#include <cstddef>
#include <vector>

namespace simxr::app::windows
{
class PrivateSecurity
{
public:
	explicit PrivateSecurity(bool directory, bool inherit = false);
	~PrivateSecurity();
	PrivateSecurity(const PrivateSecurity&) = delete;
	PrivateSecurity& operator=(const PrivateSecurity&) = delete;

	SECURITY_ATTRIBUTES* Attributes()
	{
		return &m_attributes;
	}

	const std::wstring& SidText() const
	{
		return m_sidText;
	}

	void Check(HANDLE handle, SE_OBJECT_TYPE type, bool requireProtected) const;

private:
	std::vector<std::byte> m_sid;
	std::wstring m_sidText;
	PSECURITY_DESCRIPTOR m_descriptor = nullptr;
	SECURITY_ATTRIBUTES m_attributes{};
};
} // namespace simxr::app::windows
