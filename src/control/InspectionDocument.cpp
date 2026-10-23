#include "control/InspectionDocument.h"
#include "Pch.h"

#include <limits>
#include <stdexcept>

namespace simxr::control
{
static InspectionValue Scalar(InspectionType type, Field field)
{
	return {type, std::move(field.value)};
}
InspectionValue InspectionValue::Null()
{
	return {};
}
InspectionValue InspectionValue::Boolean(bool value)
{
	return Scalar(InspectionType::Boolean, Field::Bool(1, value));
}
InspectionValue InspectionValue::Unsigned(std::uint64_t value)
{
	return Scalar(InspectionType::Unsigned, Field::U64(1, value));
}
InspectionValue InspectionValue::Signed(std::int64_t value)
{
	return Scalar(InspectionType::Signed, Field::I64(1, value));
}
InspectionValue InspectionValue::Float(float value)
{
	return Scalar(InspectionType::Float, Field::F32(1, value));
}
InspectionValue InspectionValue::Text(std::string_view value)
{
	return Scalar(InspectionType::Text, Field::Text(1, value));
}
InspectionValue InspectionValue::Revision(std::uint64_t value)
{
	if (!value)
		throw std::invalid_argument("Revision IDs must be nonzero");
	return Scalar(InspectionType::Revision, Field::U64(1, value));
}
std::vector<std::byte> InspectionValue::Encode(std::string_view name) const
{
	return EncodeFields(std::array{Field::Text(1, name, true), Field::U32(2, static_cast<std::uint32_t>(type), true),
								   Field::Bytes(3, data, true)});
}
InspectionValue InspectionValue::Object(std::vector<std::pair<std::string, InspectionValue>> fields)
{
	std::vector<Field> result;
	result.reserve(fields.size());
	for (const auto& [name, value] : fields)
	{
		if (name.empty() || std::count_if(fields.begin(), fields.end(),
										  [&](const auto& field)
										  {
											  return field.first == name;
										  }) != 1)
			throw std::invalid_argument("Inspection object names must be distinct and nonempty");
		if (result.size() == MaxFields)
			throw std::length_error("Inspection object field capacity exceeded");
		result.push_back(Field::Bytes(static_cast<std::uint16_t>(result.size() + 1), value.Encode(name), true));
	}
	return {InspectionType::Object, EncodeFields(result)};
}
InspectionValue InspectionValue::Array(std::vector<InspectionValue> values)
{
	if (values.size() > MaxFields * MaxFields)
		throw std::length_error("Inspection array capacity exceeded");
	std::vector<Field> pages;
	for (std::size_t begin = 0; begin < values.size(); begin += MaxFields)
	{
		std::vector<Field> page;
		const auto count = std::min(MaxFields, values.size() - begin);
		page.reserve(count);
		for (std::size_t index = 0; index < count; ++index)
			page.push_back(Field::Bytes(static_cast<std::uint16_t>(index + 1), values[begin + index].Encode(), true));
		pages.push_back(Field::Bytes(static_cast<std::uint16_t>(pages.size() + 1), EncodeFields(page), true));
	}
	return {InspectionType::Array, EncodeFields(pages)};
}
InspectionStore::InspectionStore(std::size_t frames, std::size_t revisions, std::size_t bytes)
	: m_frameCapacity(frames), m_revisionCapacity(revisions), m_byteCapacity(bytes)
{
	if (!frames || frames > 256 || !revisions || revisions > 4096 || !bytes || bytes > 64 * 1024 * 1024)
		throw std::invalid_argument("Invalid inspection catalog capacity");
	m_records.reserve(frames);
	m_readers.reserve(frames);
	m_revisions.reserve(revisions);
}
std::shared_ptr<const InspectionRecord> InspectionStore::Find(ControlSessionId session, FrameKey frame) const
{
	for (const auto& record : m_records)
		if (record->session == session && record->frame == frame)
			return record;
	return {};
}
std::shared_ptr<const InspectionRecord> InspectionStore::Read(std::uint64_t id) const
{
	for (const auto& record : m_records)
		if (record->id == id)
			return record;
	return {};
}
std::shared_ptr<const FeatureRevision> InspectionStore::ReadRevision(std::uint64_t id) const
{
	for (const auto& record : m_records)
		for (const auto& revision : record->revisions)
			if (revision->id == id)
				return revision;
	return {};
}
std::shared_ptr<const InspectionRecord>
InspectionStore::Retain(ControlSessionId session, FrameKey frame, std::vector<FeatureDocument> features,
						const std::function<std::vector<std::byte>(std::span<const std::uint64_t>)>& serialize)
{
	if (!session.value || !frame.generation.value || !frame.frame.value)
		throw std::invalid_argument("Invalid inspection identity");
	if (features.size() > m_revisionCapacity)
		throw std::length_error("Inspection feature-reference capacity exceeded");
	if (auto existing = Find(session, frame))
		return existing;
	std::erase_if(m_readers,
				  [](const auto& reader)
				  {
					  return reader.expired();
				  });
	std::erase_if(m_revisions,
				  [](const auto& revision)
				  {
					  return revision.expired();
				  });
	if (m_nextFrame == std::numeric_limits<std::uint64_t>::max())
		throw std::length_error("Inspection identity exhausted");
	std::vector<std::shared_ptr<const InspectionRecord>> liveReaders;
	std::vector<std::shared_ptr<const FeatureRevision>> knownRevisions;
	std::vector<std::shared_ptr<const FeatureRevision>> addedRevisions;
	std::vector<std::shared_ptr<const FeatureRevision>> selectedRevisions;
	std::vector<std::uint64_t> revisionIds;
	std::size_t usedBytes = 0;
	for (const auto& weak : m_readers)
		if (auto reader = weak.lock())
		{
			usedBytes += reader->document.size();
			liveReaders.push_back(std::move(reader));
		}
	for (const auto& weak : m_revisions)
		if (auto revision = weak.lock())
		{
			usedBytes += revision->document.bytes.size();
			knownRevisions.push_back(std::move(revision));
		}
	// External shared readers may disappear between the first prune and weak
	// lock. Drop those failed locks now; strong snapshots above keep every
	// surviving entry stable through the no-allocation publication below.
	std::erase_if(m_readers,
				  [](const auto& reader)
				  {
					  return reader.expired();
				  });
	std::erase_if(m_revisions,
				  [](const auto& revision)
				  {
					  return revision.expired();
				  });
	if (m_readers.size() >= m_frameCapacity)
		throw std::length_error("Inspection frame capacity exceeded");
	selectedRevisions.reserve(features.size());
	revisionIds.reserve(features.size());
	auto nextRevision = m_nextRevision;
	for (auto& feature : features)
	{
		if (feature.kind.empty() || feature.kind.size() > 64 || feature.bytes.size() > MaxPayload - 4096)
			throw std::invalid_argument("Invalid feature revision");
		std::shared_ptr<const FeatureRevision> selectedRevision;
		const auto match = [&](const auto& revision)
		{
			return revision->document.kind == feature.kind && revision->document.bytes == feature.bytes;
		};
		for (const auto& revision : knownRevisions)
			if (match(revision))
			{
				selectedRevision = revision;
				break;
			}
		if (!selectedRevision)
			for (const auto& revision : addedRevisions)
				if (match(revision))
				{
					selectedRevision = revision;
					break;
				}
		if (!selectedRevision)
		{
			if (knownRevisions.size() + addedRevisions.size() >= m_revisionCapacity ||
				nextRevision == std::numeric_limits<std::uint64_t>::max())
				throw std::length_error("Inspection revision capacity exceeded");
			if (usedBytes > m_byteCapacity || feature.bytes.size() > m_byteCapacity - usedBytes)
				throw std::length_error("Inspection byte capacity exceeded");
			selectedRevision =
				std::make_shared<const FeatureRevision>(FeatureRevision{nextRevision++, std::move(feature)});
			usedBytes += selectedRevision->document.bytes.size();
			addedRevisions.push_back(selectedRevision);
		}
		revisionIds.push_back(selectedRevision->id);
		selectedRevisions.push_back(std::move(selectedRevision));
	}
	auto document = serialize(revisionIds);
	// Leave room for the response envelope and its metadata fields.
	if (document.size() > MaxPayload - 4096 || usedBytes > m_byteCapacity ||
		document.size() > m_byteCapacity - usedBytes)
		throw std::length_error("Inspection byte capacity exceeded");
	auto record = std::make_shared<const InspectionRecord>(
		InspectionRecord{m_nextFrame, session, frame, std::move(document), std::move(selectedRevisions)});
	// Vectors were reserved to their hard capacities in the constructor. Every
	// allocation, including caller serialization, precedes these no-throw commits.
	for (const auto& revision : addedRevisions)
		m_revisions.push_back(revision);
	m_records.push_back(record);
	m_readers.push_back(record);
	++m_nextFrame;
	m_nextRevision = nextRevision;
	return record;
}
bool InspectionStore::Release(std::uint64_t id) noexcept
{
	const auto previous = m_records.size();
	std::erase_if(m_records,
				  [&](const auto& record)
				  {
					  return record->id == id;
				  });
	return previous != m_records.size();
}
} // namespace simxr::control
