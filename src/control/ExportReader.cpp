#include "Pch.h"
#include "control/ExportReader.h"
#include "control/ControlSchema.h"

namespace simxr::control
{
static std::uint64_t RequiredNumber(const Message& message, std::uint16_t tag)
{
	const auto* field = message.Find(tag);
	if (!field || !field->AsU64())
		throw std::runtime_error("Export response omitted a required number");
	return *field->AsU64();
}

ExportReader::ExportReader(Exchange exchange, std::vector<Field> fields, const std::function<void()>& wait)
	: m_exchange(std::move(exchange))
{
	m_id = RequiredNumber(m_exchange(OpCode::StartExport, std::move(fields)), tag::ExportId);
	if (!m_id)
		throw std::runtime_error("Runtime omitted the export identity");
	try
	{
		for (;;)
		{
			m_info = m_exchange(OpCode::ExportInfo, {Field::U64(tag::ExportId, m_id, true)});
			const auto* field = m_info.Find(tag::Phase);
			const auto phase = field ? field->AsText() : std::nullopt;
			if (phase == "complete")
				break;
			if (phase != "pending")
				throw std::runtime_error("Image export failed");
			wait();
		}
		m_size = RequiredNumber(m_info, tag::TotalBytes);
	}
	catch (...)
	{
		Cleanup();
		throw;
	}
}

ExportReader::~ExportReader()
{
	Cleanup();
}

void ExportReader::Cleanup() noexcept
{
	try
	{
		Release();
	}
	catch (...)
	{}
}

void ExportReader::Release()
{
	if (m_id)
	{
		m_exchange(OpCode::ReleaseExport, {Field::U64(tag::ExportId, m_id, true)});
		m_id = 0;
	}
}

void ExportReader::Read(const std::function<void(std::span<const std::byte>)>& consume)
{
	if (!m_id)
		throw std::logic_error("Export was already released");
	Read(m_exchange, m_id, m_size, consume);
}

void ExportReader::Read(const Exchange& exchange, std::uint64_t ticket, std::uint64_t size,
						const std::function<void(std::span<const std::byte>)>& consume)
{
	if (!ticket)
		throw std::invalid_argument("Export identity must be nonzero");
	for (std::uint64_t offset = 0; offset < size;)
	{
		const auto length = std::min<std::uint64_t>(MaxPayload - 4096, size - offset);
		const auto chunk = exchange(OpCode::ReadExport,
									{Field::U64(tag::ExportId, ticket, true), Field::U64(tag::Offset, offset, true),
									 Field::U64(tag::Length, length, true)});
		const auto* bytes = chunk.Find(tag::Bytes);
		if (!bytes || bytes->value.empty() || RequiredNumber(chunk, tag::Offset) != offset ||
			bytes->value.size() > length)
			throw std::runtime_error("Invalid export chunk");
		consume(bytes->value);
		offset += bytes->value.size();
	}
}
}
