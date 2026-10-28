#pragma once

#include "app/services/HeadsetHost.h"

#include <memory>
#include <optional>
#include <string>

namespace simxr::app
{
struct HeadsetPageRequest
{
	bool connect = false;
	bool realign = false;
	HeadsetHostOptions options;
};
class PhysicalHeadsetPage
{
  public:
	std::optional<HeadsetPageRequest> Draw(const HeadsetHostSnapshot& host, bool canConnect,
										   const std::string& conflict = {}, const ObservedSetup* game = nullptr);
	void SetError(std::string error);
	bool TrackersEnabled() const
	{
		return m_trackers;
	}

	bool BodyEnabled() const
	{
		return m_body != 0;
	}

  private:
	struct FileResult;
	std::shared_ptr<FileResult> m_file;
	std::string m_manifest, m_error;
	bool m_customRuntime = false, m_calibrateLocal = false, m_trackers = false;
	int m_body = 0;
	float m_eyeHeight = 1.7f;
};
} // namespace simxr::app
