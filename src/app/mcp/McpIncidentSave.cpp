#include "app/mcp/McpPch.h"
#include "app/mcp/McpIncidentSave.h"
#include "app/services/ImageExport.h"
#include "app/services/Utf8Path.h"
#include "graphics/ColorPixels.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;

std::filesystem::path IncidentDirectory(const Json& arguments)
{
	if (!arguments.contains("directory"))
		return {};
	const auto& value = arguments.at("directory");
	if (!value.is_string())
		throw std::invalid_argument("directory must be an absolute path to a new directory");
	const auto& text = value.get_ref<const std::string&>();
	if (text.empty() || text.size() > 4096 || text.find('\0') != std::string::npos)
		throw std::invalid_argument("Invalid incident directory");
	const auto path = Utf8Path(text);
	if (!path.is_absolute() || path.filename().empty() || path.filename() == "." || path.filename() == "..")
		throw std::invalid_argument("directory must be an absolute path to a new directory");
	if (std::filesystem::exists(path))
		throw std::invalid_argument("Incident directory already exists");
	if (!std::filesystem::is_directory(path.parent_path()))
		throw std::invalid_argument("Incident directory's parent does not exist");
	return path;
}

class IncidentFiles
{
public:
	explicit IncidentFiles(std::filesystem::path directory) : m_directory(std::move(directory))
	{
		if (!std::filesystem::create_directory(m_directory))
			throw std::runtime_error("Incident directory already exists");
	}

	~IncidentFiles()
	{
		if (m_complete)
			return;
		std::error_code ignored;
		for (const auto& file : m_files)
			std::filesystem::remove(file, ignored);
		std::filesystem::remove(m_directory, ignored);
	}

	void Write(const std::filesystem::path& name, std::span<const std::byte> bytes, const std::function<void()>& check)
	{
		check();
		const auto destination = m_directory / name;
		m_files.push_back(destination);
		try
		{
			SaveExportFile(destination, bytes, false, check);
		}
		catch (...)
		{
			m_files.pop_back();
			throw;
		}
	}

	void Complete()
	{
		m_complete = true;
	}

private:
	std::filesystem::path m_directory;
	std::vector<std::filesystem::path> m_files;
	bool m_complete = false;
};

void SaveIncident(const std::filesystem::path& directory, Json& metadata, std::span<const std::byte> jpeg,
				  const std::vector<std::vector<std::byte>>& frames, unsigned width, unsigned height,
				  const std::function<void()>& check)
{
	if (frames.size() != metadata.at("window").at("entries").size() || frames.empty() || frames.size() > 24 ||
		width != 320 || height != 104)
		throw std::runtime_error("Invalid incident archive frames");
	IncidentFiles files(directory);
	files.Write("contact.jpg", jpeg, check);
	Json saved = Json::array();
	for (std::size_t index = 0; index < frames.size(); ++index)
	{
		check();
		if (frames[index].size() != std::size_t(width) * height * 4)
			throw std::runtime_error("Invalid incident archive frame dimensions");
		const auto image =
			EncodeImage(frames[index], static_cast<std::uint32_t>(ColorFormat::Rgba8Unorm), width, height, true, 85);
		const auto filename = "frame-" + std::to_string(index) + ".png";
		files.Write(filename, image.bytes, check);
		const auto& entry = metadata.at("window").at("entries").at(index);
		saved.push_back({{"frame_id", entry.at("frame_id")}, {"phase", entry.at("phase")}, {"file", filename}});
	}
	metadata["directory"] = PathText(directory);
	metadata["saved_frames"] = std::move(saved);
	metadata["contact_sheet"] = "contact.jpg";
	metadata["archive_version"] = 1;
	const auto manifest = metadata.dump(2);
	files.Write("manifest.json", std::as_bytes(std::span(manifest.data(), manifest.size())), check);
	files.Complete();
}
}
