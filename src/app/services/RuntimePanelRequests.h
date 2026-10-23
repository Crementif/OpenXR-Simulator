#pragma once

#include "app/RuntimePanelProtocol.h"
#include "app/services/RuntimeConnection.h"

#include <array>
#include <map>

namespace simxr::app
{
struct PanelReply
{
	PanelTask task;
	RuntimeReply response;
};
// UI-thread admission bookkeeping; RuntimeConnection owns every socket operation.
// Request identity and its source scope are prepared before transport acceptance.
class RuntimePanelRequests
{
  public:
	void Connect(std::string path);
	void Disconnect();
	void Close() noexcept;
	RuntimeConnectionSnapshot Snapshot() const;
	bool RefreshEpoch();
	std::uint64_t Epoch() const noexcept;
	std::vector<PanelReply> Poll();
	std::uint64_t Send(control::OpCode code, PanelWork kind, PanelFields fields = {}, std::uint64_t id = 0,
					   std::vector<std::shared_ptr<control::TransferHandle>> handles = {});
	bool Has(PanelWork kind, std::uint64_t id = 0) const;
	bool Pending() const noexcept;
	void Release(control::OpCode code, std::uint16_t field, std::uint64_t id, std::uint64_t session = 0,
				 std::uint64_t generation = 0);
	void FlushReleases();
	std::string TakeError();

  private:
	struct ReleaseRequest
	{
		control::OpCode code = control::OpCode::ReleaseImage;
		std::uint16_t field = 0;
		std::uint64_t id = 0, epoch = 0, session = 0, generation = 0;
	};
	RuntimeConnection m_connection;
	std::map<std::uint64_t, PanelTask> m_tasks;
	std::array<ReleaseRequest, 64> m_releases{};
	std::uint64_t m_epoch = 0;
	std::string m_error;
};
} // namespace simxr::app
