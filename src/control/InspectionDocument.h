#pragma once

#include "control/WireProtocol.h"
#include "presentation/FrameTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace simxr::control
{
// Stable inspection-tree types. Objects contain named children; arrays contain
// pages of at most 256 unnamed children. All scalar data is little endian.
enum class InspectionType : std::uint32_t
{
	Null = 0,
	Boolean = 1,
	Unsigned = 2,
	Signed = 3,
	Float = 4,
	Text = 5,
	Object = 6,
	Array = 7,
	Revision = 8
};

struct InspectionValue
{
	InspectionType type = InspectionType::Null;
	std::vector<std::byte> data;
	static InspectionValue Null();
	static InspectionValue Boolean(bool value);
	static InspectionValue Unsigned(std::uint64_t value);
	static InspectionValue Signed(std::int64_t value);
	static InspectionValue Float(float value);
	static InspectionValue Text(std::string_view value);
	static InspectionValue Revision(std::uint64_t value);
	static InspectionValue Object(std::vector<std::pair<std::string, InspectionValue>> fields);
	static InspectionValue Array(std::vector<InspectionValue> values);
	std::vector<std::byte> Encode(std::string_view name = {}) const;
};

struct FeatureDocument
{
	std::string kind;
	std::vector<std::byte> bytes;
};

struct FeatureRevision
{
	std::uint64_t id = 0;
	FeatureDocument document;
};

struct InspectionRecord
{
	std::uint64_t id = 0;
	ControlSessionId session;
	FrameKey frame;
	std::vector<std::byte> document;
	std::vector<std::shared_ptr<const FeatureRevision>> revisions;
};

// Connection-owned, single-flight catalog. Capacity includes readers retained
// after Release. A failed publication changes neither IDs nor visible records.
class InspectionStore
{
public:
	explicit InspectionStore(std::size_t frames = 32, std::size_t revisions = 64, std::size_t bytes = 8 * 1024 * 1024);
	std::shared_ptr<const InspectionRecord> Find(ControlSessionId session, FrameKey frame) const;
	std::shared_ptr<const InspectionRecord> Read(std::uint64_t id) const;
	std::shared_ptr<const FeatureRevision> ReadRevision(std::uint64_t id) const;
	std::shared_ptr<const InspectionRecord>
	Retain(ControlSessionId session, FrameKey frame, std::vector<FeatureDocument> features,
		   const std::function<std::vector<std::byte>(std::span<const std::uint64_t>)>& serialize);
	bool Release(std::uint64_t id) noexcept;

private:
	std::size_t m_frameCapacity;
	std::size_t m_revisionCapacity;
	std::size_t m_byteCapacity;
	std::uint64_t m_nextFrame = 1;
	std::uint64_t m_nextRevision = 1;
	std::vector<std::shared_ptr<const InspectionRecord>> m_records;
	std::vector<std::weak_ptr<const InspectionRecord>> m_readers;
	std::vector<std::weak_ptr<const FeatureRevision>> m_revisions;
};
} // namespace simxr::control
