#include "app/RuntimePanelRequests.h"

#include <algorithm>
#include <utility>

namespace simxr::app
{
void RuntimePanelRequests::Connect(std::string path)
{
	m_connection.Connect(std::move(path));
}
void RuntimePanelRequests::Disconnect()
{
	m_connection.Disconnect();
}
void RuntimePanelRequests::Close() noexcept
{
	m_connection.Close();
}
RuntimeConnectionSnapshot RuntimePanelRequests::Snapshot() const
{
	return m_connection.Snapshot();
}
std::uint64_t RuntimePanelRequests::Epoch() const noexcept
{
	return m_epoch;
}
bool RuntimePanelRequests::Pending() const noexcept
{
	return !m_tasks.empty();
}
bool RuntimePanelRequests::RefreshEpoch()
{
	const auto epoch = m_connection.Snapshot().epoch;
	if (epoch == m_epoch)
		return false;
	m_releases.fill({});
	m_tasks.clear();
	m_epoch = epoch;
	return true;
}
std::vector<PanelReply> RuntimePanelRequests::Poll()
{
	std::vector<PanelReply> replies;
	// RuntimeConnection admits at most 32 unpolled replies. Allocate before
	// transferring their FD ownership out of the transport.
	replies.reserve(32);
	auto received = m_connection.Poll();
	for (auto& response : received)
	{
		const auto found = m_tasks.find(response.ticket);
		if (found == m_tasks.end())
			continue;
		const auto task = found->second;
		m_tasks.erase(found);
		if (response.epoch == m_epoch)
			replies.push_back({task, std::move(response)});
	}
	return replies;
}
std::uint64_t RuntimePanelRequests::Send(control::OpCode code, PanelWork kind, PanelFields fields, std::uint64_t id,
										 std::vector<std::shared_ptr<control::TransferHandle>> handles)
{
	control::ControlPacket packet;
	packet.message.opcode = static_cast<std::uint16_t>(code);
	packet.message.fields = std::move(fields);
	packet.handles = std::move(handles);
	auto entry = m_tasks.emplace(0, PanelTask{kind, id, panel::U64(packet.message, control::tag::Session),
											  panel::U64(packet.message, control::tag::Generation)});
	try
	{
		const auto ticket = m_connection.Submit(std::move(packet));
		if (!ticket)
		{
			m_tasks.erase(entry.first);
			return 0;
		}
		auto node = m_tasks.extract(entry.first);
		node.key() = ticket;
		m_tasks.insert(std::move(node));
		return ticket;
	}
	catch (...)
	{
		m_tasks.erase(0);
		throw;
	}
}
bool RuntimePanelRequests::Has(PanelWork kind, std::uint64_t id) const
{
	return std::any_of(m_tasks.begin(), m_tasks.end(),
					   [&](const auto& entry)
					   {
						   return entry.second.kind == kind && (!id || entry.second.id == id);
					   });
}
void RuntimePanelRequests::Release(control::OpCode code, std::uint16_t field, std::uint64_t id)
{
	if (!id)
		return;
	for (const auto& pending : m_releases)
		if (pending.id == id && pending.code == code && pending.epoch == m_epoch)
			return;
	const auto entry = std::find_if(m_releases.begin(), m_releases.end(),
									[](const auto& pending)
									{
										return !pending.id;
									});
	if (entry == m_releases.end())
	{
		m_connection.Disconnect(m_epoch);
		m_error = "Resource release queue exhausted; connection closed to release its tables";
		return;
	}
	*entry = {code, field, id, m_epoch};
}
void RuntimePanelRequests::FlushReleases()
{
	for (auto& pending : m_releases)
	{
		if (!pending.id || pending.epoch != m_epoch)
			continue;
		if (!Send(pending.code, PanelWork::Release, {control::Field::U64(pending.field, pending.id, true)}))
			break;
		pending = {};
	}
}
std::string RuntimePanelRequests::TakeError()
{
	return std::exchange(m_error, {});
}
} // namespace simxr::app
