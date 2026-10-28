#include "app/UiPch.h"
#include "app/services/UpdateChecker.h"

#include <curl/curl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>

namespace simxr::app
{
class CurlLibrary
{
  public:
	CurlLibrary()
	{
		m_library = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
		if (!m_library)
			throw std::runtime_error("System libcurl is unavailable");
		try
		{
			Load(globalInit, "curl_global_init");
			Load(versionInfo, "curl_version_info");
			Load(init, "curl_easy_init");
			Load(setopt, "curl_easy_setopt");
			Load(perform, "curl_easy_perform");
			Load(getinfo, "curl_easy_getinfo");
			Load(cleanup, "curl_easy_cleanup");
			if (globalInit(CURL_GLOBAL_DEFAULT) != CURLE_OK)
				throw std::runtime_error("Cannot initialize system libcurl");
			const auto* version = versionInfo(CURLVERSION_FIRST);
			if (!version || !(version->features & CURL_VERSION_ASYNCHDNS))
				throw std::runtime_error("System libcurl does not support bounded DNS lookups");
		}
		catch (...)
		{
			dlclose(m_library);
			m_library = nullptr;
			throw;
		}
	}
	~CurlLibrary()
	{
		if (m_library)
			dlclose(m_library);
	}
	decltype(&curl_easy_init) init = nullptr;
	decltype(&curl_easy_setopt) setopt = nullptr;
	decltype(&curl_easy_perform) perform = nullptr;
	decltype(&curl_easy_getinfo) getinfo = nullptr;
	decltype(&curl_easy_cleanup) cleanup = nullptr;

  private:
	template <class T> void Load(T& target, const char* name)
	{
		target = reinterpret_cast<T>(dlsym(m_library, name));
		if (!target)
			throw std::runtime_error("System libcurl is missing a required function");
	}
	void* m_library = nullptr;
	decltype(&curl_global_init) globalInit = nullptr;
	decltype(&curl_version_info) versionInfo = nullptr;
};
static std::size_t AppendRelease(char* data, std::size_t size, std::size_t count, void* context) noexcept
{
	try
	{
		if (size && count > (1024 * 1024) / size)
			return 0;
		const auto bytes = size * count;
		auto& response = *static_cast<std::string*>(context);
		if (bytes > 1024 * 1024 - response.size())
			return 0;
		response.append(data, bytes);
		return bytes;
	}
	catch (...)
	{
		return 0;
	}
}
std::string FetchLatestRelease(std::string_view repository)
{
	if (!ValidReleaseRepository(repository))
		throw std::invalid_argument("A public GitHub release repository is not configured");
	// A TLS backend can raise SIGPIPE; this short-lived worker keeps it blocked until it exits.
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGPIPE);
	if (pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0)
		throw std::runtime_error("Cannot protect the update worker from SIGPIPE");
	static CurlLibrary library;
	using Handle = std::unique_ptr<CURL, decltype(library.cleanup)>;
	Handle handle(library.init(), library.cleanup);
	if (!handle)
		throw std::runtime_error("Cannot start an update request");
	const auto url = "https://api.github.com/repos/" + std::string(repository) + "/releases/latest";
	std::string response;
	const auto option = [&](CURLoption name, auto value)
	{
		if (library.setopt(handle.get(), name, value) != CURLE_OK)
			throw std::runtime_error("System libcurl does not support update requests");
	};
	option(CURLOPT_URL, url.c_str());
	option(CURLOPT_USERAGENT, "OpenXR-Simulator update check");
	option(CURLOPT_NOSIGNAL, 1L);
	option(CURLOPT_CONNECTTIMEOUT_MS, 2000L);
	option(CURLOPT_TIMEOUT_MS, 5000L);
	option(CURLOPT_FOLLOWLOCATION, 0L);
	option(CURLOPT_SSL_VERIFYPEER, 1L);
	option(CURLOPT_SSL_VERIFYHOST, 2L);
	option(CURLOPT_WRITEFUNCTION, &AppendRelease);
	option(CURLOPT_WRITEDATA, &response);
	if (library.perform(handle.get()) != CURLE_OK)
		throw std::runtime_error("Cannot contact GitHub releases");
	long status = 0;
	if (library.getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status) != CURLE_OK || status != 200)
		throw std::runtime_error("GitHub releases returned HTTP " + std::to_string(status));
	return response;
}
} // namespace simxr::app
