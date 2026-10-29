#pragma once

#include "control/WireProtocol.h"

#include <functional>
#include <span>
#include <vector>

namespace simxr::control
{
class ExportReader
{
  public:
	// The caller checks response errors and bounds exchanges. Cancellation must
	// allow release or close the owning connection to reclaim retained exports.
	using Exchange = std::function<Message(OpCode, std::vector<Field>)>;
	ExportReader(Exchange exchange, std::vector<Field> fields, const std::function<void()>& wait);
	~ExportReader();
	ExportReader(const ExportReader&) = delete;
	ExportReader& operator=(const ExportReader&) = delete;
	const Message& Info() const { return m_info; }
	std::uint64_t Id() const { return m_id; }
	std::uint64_t Size() const { return m_size; }
	void Read(const std::function<void(std::span<const std::byte>)>& consume);
	void Release();
	// Read a completed caller-owned ticket without taking or releasing its ownership.
	static void Read(const Exchange& exchange, std::uint64_t ticket, std::uint64_t size,
		const std::function<void(std::span<const std::byte>)>& consume);

  private:
	void Cleanup() noexcept;
	Exchange m_exchange;
	std::uint64_t m_id = 0, m_size = 0;
	Message m_info;
};
}
