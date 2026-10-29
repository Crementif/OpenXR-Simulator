#include "app/services/LibraryCatalog.h"
#include "Pch.h"
#include "app/platform/FileStorage.h"
#include "app/services/RecordingController.h"
#include "app/services/Utf8Path.h"
#include "automation/RecordingArchive.h"
#include "automation/RecordingRange.h"

namespace simxr::app
{
static void ReadEntry(LibraryEntry& entry, const std::filesystem::path& path)
{
	try
	{
		constexpr std::string_view signature = "-- OpenXR Simulator recorded inputs";
		std::array<char, signature.size()> prefix{};
		std::ifstream input(path, std::ios::binary);
		if (!input)
			throw std::runtime_error("Cannot open file");
		input.read(prefix.data(), prefix.size());
		if (input.bad() || (input.fail() && !input.eof()))
			throw std::runtime_error("Cannot read file");
		entry.take = std::string_view(prefix.data(), static_cast<std::size_t>(input.gcount())) == signature;
		input.clear();
		input.close();
		if (!input)
			throw std::runtime_error("Cannot close file");
		if (entry.take)
			entry.seed = OpenSavedRecording(entry.path).seed;
	}
	catch (const std::exception& error)
	{
		entry.error = error.what();
	}
}

LibraryCatalog::LibraryCatalog(std::filesystem::path root) : m_root(std::move(root)) {}

LibraryCatalog::~LibraryCatalog()
{
	m_worker.request_stop();
	m_changed.notify_all();
	if (m_worker.joinable())
		m_worker.join();
}

bool LibraryCatalog::ValidName(const std::string& name)
{
	return !name.empty() && name != "." && name != ".." && name.back() != '.' && name.back() != ' ' &&
		   name.find_first_of("/\\:<>\"|?*") == std::string::npos &&
		   std::none_of(name.begin(), name.end(), [](unsigned char character) { return character < 32; });
}

void LibraryCatalog::StartWorker()
{
	if (!m_worker.joinable())
		m_worker = std::jthread([this](std::stop_token stop) { Run(stop); });
}

void LibraryCatalog::Refresh()
{
	std::lock_guard lock(m_mutex);
	StartWorker();
	m_refresh = true;
	m_changed.notify_all();
}

bool LibraryCatalog::Submit(Job job)
{
	std::lock_guard lock(m_mutex);
	if (m_busy || m_job || m_result)
		return false;
	StartWorker();
	m_job = std::move(job);
	m_changed.notify_all();
	return true;
}

bool LibraryCatalog::Rename(std::string path, std::string name)
{
	return Submit({LibraryAction::Rename, std::move(path), std::move(name)});
}

bool LibraryCatalog::Delete(std::string path)
{
	return Submit({LibraryAction::Delete, std::move(path), {}});
}

bool LibraryCatalog::ExportRange(std::string path, std::string name, std::uint64_t first, std::uint64_t last)
{
	return Submit({LibraryAction::ExportRange, std::move(path), std::move(name), first, last});
}

bool LibraryCatalog::ReadEvidence(std::string path, std::uint64_t cursor)
{
	return Submit({LibraryAction::Evidence, std::move(path), {}, cursor});
}

bool LibraryCatalog::Busy() const
{
	std::lock_guard lock(m_mutex);
	return m_busy || m_refresh || m_job.has_value() || m_result.has_value();
}

std::optional<LibraryResult> LibraryCatalog::TakeResult()
{
	std::lock_guard lock(m_mutex);
	auto result = std::exchange(m_result, {});
	m_changed.notify_all();
	return result;
}

void LibraryCatalog::Scan(LibraryResult& result, std::stop_token stop)
{
	std::vector<LibraryEntry> entries;
	constexpr std::size_t capacity = 512;
	for (const auto& directory : {m_root / "recordings", m_root / "scripts"})
	{
		if (stop.stop_requested())
			return;
		if (!std::filesystem::exists(directory))
			continue;
		for (const auto& file : std::filesystem::directory_iterator(directory))
		{
			if (stop.stop_requested())
				return;
			const bool archive = file.path().extension() == ".simxrtake" && file.is_directory() && !file.is_symlink();
			if (!archive && (file.path().extension() != ".luau" || !file.is_regular_file()))
				continue;
			if (entries.size() == capacity)
			{
				result.limited = true;
				break;
			}
			LibraryEntry entry;
			entry.path = PathText(file.path());
			entry.name = PathText(file.path().stem());
			entry.archive = archive;
			if (archive)
			{
				entry.take = true;
				try
				{
					const auto input = file.path() / "input.simxr";
					entry.modified = std::filesystem::last_write_time(input);
					entry.bytes = std::filesystem::file_size(input);
					const auto cached = std::ranges::find_if(m_archiveCache, [&](const auto& previous) {
						return previous.path == entry.path && previous.bytes == entry.bytes &&
							   previous.modified == entry.modified;
					});
					if (cached != m_archiveCache.end())
					{
						entries.push_back(*cached);
						continue;
					}
					automation::RecordingArchiveReader reader(file.path());
					entry.seed = reader.State().origin.seed;
					entry.firstFrame = entry.lastFrame = reader.State().origin.originFrame.value;
					while (const auto page = reader.Next(stop))
					{
						if (stop.stop_requested())
							return;
						for (const auto& transaction : page->recording.transactions)
							if (transaction.phase == "applied")
								entry.lastFrame = std::max(entry.lastFrame, transaction.frame.value);
					}
					if (!reader.State().complete)
						entry.error = "Take is still recording or incomplete";
				}
				catch (const std::exception& error)
				{
					entry.error = error.what();
				}
			}
			else
			{
				entry.modified = file.last_write_time();
				entry.bytes = file.file_size();
				ReadEntry(entry, file.path());
			}
			entries.push_back(std::move(entry));
		}
		if (result.limited)
			break;
	}
	std::ranges::sort(entries, [](const auto& a, const auto& b) {
		return a.modified == b.modified ? a.path < b.path : a.modified > b.modified;
	});
	m_archiveCache.clear();
	for (const auto& entry : entries)
		if (entry.archive && entry.error.empty())
			m_archiveCache.push_back(entry);
	result.entries = std::move(entries);
}

void LibraryCatalog::Run(std::stop_token stop)
{
	for (;;)
	{
		Job job{LibraryAction::Scan, {}, {}};
		{
			std::unique_lock lock(m_mutex);
			if (!m_changed.wait(lock, stop, [this] { return !m_result && (m_job || m_refresh); }))
				return;
			if (m_job)
				job = std::move(*m_job);
			m_job.reset();
			m_refresh = false;
			m_busy = true;
		}
		LibraryResult result;
		result.action = job.action;
		result.path = std::move(job.path);
		try
		{
			if (job.action != LibraryAction::Scan)
			{
				if (result.path.empty() || result.path.find('\0') != std::string::npos)
					throw std::invalid_argument("Invalid library file path");
				const auto source = Utf8Path(result.path);
				const auto status = std::filesystem::symlink_status(source);
				const bool archive = source.extension() == ".simxrtake" && std::filesystem::is_directory(status);
				if (!std::filesystem::is_regular_file(status) && !std::filesystem::is_symlink(status) && !archive)
					throw std::runtime_error("The selected file is missing or is a directory");
				if (job.action == LibraryAction::Evidence)
				{
					if (!archive)
						throw std::runtime_error("SDK evidence requires an archive");
					result.evidence = automation::ReadRecordingEvidence(source, job.first, stop);
				}
				else if (job.action == LibraryAction::Rename)
				{
					if (!ValidName(job.name))
						throw std::invalid_argument("Choose a valid file name without path separators");
					const auto destination =
						source.parent_path() / Utf8Path(job.name + (archive ? ".simxrtake" : ".luau"));
					result.renamed = PathText(destination);
					RenameFileWithoutReplacing(source, destination);
				}
				else if (job.action == LibraryAction::ExportRange)
				{
					if (!archive || !ValidName(job.name))
						throw std::invalid_argument("Choose an archive and a valid output name");
					const auto destination = source.parent_path() / Utf8Path(job.name + ".luau");
					const auto range =
						automation::ReadRecordingRange(source, FrameId{job.first}, FrameId{job.last}, stop);
					if (stop.stop_requested())
						throw std::runtime_error("Range export cancelled");
					SaveRecording(destination, range, false);
					result.renamed = PathText(destination);
				}
				else if (archive)
				{
					for (const auto& child : std::filesystem::directory_iterator(source))
						if (child.path().filename() != "input.simxr" || !child.is_regular_file() || child.is_symlink())
							throw std::runtime_error("Take folder contains other files; remove it manually");
					std::filesystem::remove(source / "input.simxr");
					if (!std::filesystem::remove(source))
						throw std::runtime_error("Cannot remove take folder");
				}
				else if (!std::filesystem::remove(source))
					throw std::runtime_error("The selected file no longer exists");
				result.changed = job.action != LibraryAction::Evidence;
			}
			if (job.action != LibraryAction::Evidence)
				Scan(result, stop);
		}
		catch (...)
		{
			result.error = std::current_exception();
		}
		std::lock_guard lock(m_mutex);
		m_result = std::move(result);
		m_busy = false;
	}
}
} // namespace simxr::app
