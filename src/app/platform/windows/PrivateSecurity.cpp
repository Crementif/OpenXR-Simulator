#include "Pch.h"
#include "app/platform/windows/PrivateSecurity.h"

#include <sddl.h>

namespace simxr::app::windows
{
PrivateSecurity::PrivateSecurity(bool directory, bool inherit)
{
	HANDLE rawToken = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken))
		throw Error("Cannot inspect user identity");
	Handle token(rawToken);
	DWORD bytes = 0;
	GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &bytes);
	if (!bytes || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		throw Error("Cannot inspect user SID");
	std::vector<std::byte> information(bytes);
	if (!GetTokenInformation(token.Get(), TokenUser, information.data(), bytes, &bytes))
		throw Error("Cannot read user SID");
	const auto user = reinterpret_cast<TOKEN_USER*>(information.data())->User.Sid;
	m_sid.resize(GetLengthSid(user));
	if (m_sid.empty() || !CopySid(static_cast<DWORD>(m_sid.size()), m_sid.data(), user))
		throw Error("Cannot copy user SID");
	LPWSTR rawSid = nullptr;
	if (!ConvertSidToStringSidW(m_sid.data(), &rawSid))
		throw Error("Cannot format user SID");
	const std::unique_ptr<wchar_t, decltype(&LocalFree)> text(rawSid, LocalFree);
	m_sidText = text.get();
	const auto descriptor = L"O:" + m_sidText + L"D:P(A;" + (directory ? L"OICI" : L"") + L";GA;;;" + m_sidText + L")";
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(), SDDL_REVISION_1, &m_descriptor, nullptr))
		throw Error("Cannot create private security descriptor");
	m_attributes = {sizeof(SECURITY_ATTRIBUTES), m_descriptor, inherit ? TRUE : FALSE};
}
PrivateSecurity::~PrivateSecurity()
{
	if (m_descriptor)
		LocalFree(m_descriptor);
}
void PrivateSecurity::Check(HANDLE handle, SE_OBJECT_TYPE type, bool requireProtected) const
{
	PSID owner = nullptr;
	PACL dacl = nullptr;
	PSECURITY_DESCRIPTOR raw = nullptr;
	const DWORD result = GetSecurityInfo(handle, type, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
		&owner, nullptr, &dacl, nullptr, &raw);
	if (result != ERROR_SUCCESS)
		throw Error("Cannot inspect object permissions", result);
	const std::unique_ptr<void, decltype(&LocalFree)> descriptor(raw, LocalFree);
	SECURITY_DESCRIPTOR_CONTROL control = 0;
	DWORD revision = 0;
	if (!owner || !EqualSid(owner, const_cast<std::byte*>(m_sid.data())))
		throw std::runtime_error("Private object is not owned by the current user");
	if (!dacl || dacl->AceCount != 1 || !GetSecurityDescriptorControl(raw, &control, &revision) ||
		(requireProtected && !(control & SE_DACL_PROTECTED)))
		throw std::runtime_error("Private object does not have a private access list");
	void* item = nullptr;
	if (!GetAce(dacl, 0, &item) || !item || static_cast<ACE_HEADER*>(item)->AceType != ACCESS_ALLOWED_ACE_TYPE ||
		!EqualSid(&static_cast<ACCESS_ALLOWED_ACE*>(item)->SidStart, const_cast<std::byte*>(m_sid.data())))
		throw std::runtime_error("Private object grants access to another user or group");
}
} // namespace simxr::app::windows
