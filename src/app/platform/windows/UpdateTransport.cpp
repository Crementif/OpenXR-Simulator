#include "app/UiPch.h"
#include "app/platform/windows/LaunchSupport.h"
#include "app/platform/windows/UpdateTransport.h"
#include "app/services/UpdateChecker.h"

#include <winhttp.h>

namespace simxr::app
{
class WinHttpHandle
{
public:
	explicit WinHttpHandle(HINTERNET handle) : m_handle(handle)
	{
		if (!m_handle)
			throw windows::Error("Cannot start update request");
	}

	~WinHttpHandle()
	{
		WinHttpCloseHandle(m_handle);
	}

	WinHttpHandle(const WinHttpHandle&) = delete;
	WinHttpHandle& operator=(const WinHttpHandle&) = delete;

	HINTERNET Get() const noexcept
	{
		return m_handle;
	}

private:
	HINTERNET m_handle;
};

std::string windows::FetchUpdateResponse(std::wstring_view host, std::uint16_t port, std::wstring_view path,
										 bool secure)
{
	const auto started = std::chrono::steady_clock::now();
	WinHttpHandle session(WinHttpOpen(L"OpenXR-Simulator update check",
									  secure ? WINHTTP_ACCESS_TYPE_DEFAULT_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY,
									  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	if (!WinHttpSetTimeouts(session.Get(), 2000, 2000, 2000, 2000))
		throw windows::Error("Cannot set update request timeout");
	const std::wstring hostText(host), pathText(path);
	WinHttpHandle connection(WinHttpConnect(session.Get(), hostText.c_str(), port, 0));
	WinHttpHandle request(WinHttpOpenRequest(connection.Get(), L"GET", pathText.c_str(), nullptr, WINHTTP_NO_REFERER,
											 WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
	DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
	if (!WinHttpSetOption(request.Get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)))
		throw windows::Error("Cannot configure update request");
	DWORD headerTimeout = 4000, connectionRetries = 2;
	if (!WinHttpSetOption(request.Get(), WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &headerTimeout,
						  sizeof(headerTimeout)) ||
		!WinHttpSetOption(request.Get(), WINHTTP_OPTION_CONNECT_RETRIES, &connectionRetries, sizeof(connectionRetries)))
		throw windows::Error("Cannot bound update request");
	if (!WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
		!WinHttpReceiveResponse(request.Get(), nullptr))
		throw windows::Error("Cannot contact GitHub releases");
	if (std::chrono::steady_clock::now() - started > std::chrono::seconds(8))
		throw std::runtime_error("Update request timed out");
	DWORD status = 0, statusSize = sizeof(status);
	if (!WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
							 WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
		throw windows::Error("Cannot read update response status");
	if (status != 200)
		throw std::runtime_error("GitHub releases returned HTTP " + std::to_string(status));
	std::string response;
	std::array<char, 8192> buffer{};
	while (true)
	{
		if (std::chrono::steady_clock::now() - started > std::chrono::seconds(8))
			throw std::runtime_error("Update request timed out");
		DWORD count = 0;
		if (!WinHttpReadData(request.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count))
			throw windows::Error("Cannot read update response");
		if (!count)
			break;
		if (count > 1024 * 1024 - response.size())
			throw std::runtime_error("Release response is too large");
		response.append(buffer.data(), count);
	}
	return response;
}

std::string windows::FetchLatestReleaseDirect(std::string_view repository)
{
	if (!ValidReleaseRepository(repository))
		throw std::invalid_argument("A public GitHub release repository is not configured");
	const auto path = windows::Wide("/repos/" + std::string(repository) + "/releases/latest");
	return windows::FetchUpdateResponse(L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, path, true);
}

std::string FetchLatestRelease(std::string_view repository)
{
	if (!ValidReleaseRepository(repository))
		throw std::invalid_argument("A public GitHub release repository is not configured");
	return windows::RunUpdateWorker(L"--update-worker=" + windows::Wide(repository));
}
} // namespace simxr::app
