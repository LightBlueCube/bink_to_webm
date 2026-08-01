#include "webm_assets.h"
#include "lib/plugin_logging.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>

namespace
{

std::unordered_map<std::string, std::filesystem::path> WebmAssetsList;

std::string GetLogicalName(const std::filesystem::path& path)
{
	std::string name = path.generic_string();

	static const auto unary_op = [](const unsigned char& c) -> char
	{
		if(c == '\\')
			return '/';
		return std::tolower(c);
	};
	std::transform(name.begin(), name.end(), name.begin(), unary_op);

	if(name.starts_with("media.webm/"))
		name.erase(0, sizeof("media.webm/") - 1);
	else if(name.starts_with("media/"))
		name.erase(0, sizeof("media/") - 1);

	size_t extension = name.find_last_of('.');
	size_t separator = name.find_last_of('/');
	if(extension != std::string::npos && (separator == std::string::npos || separator < extension))
		name.erase(extension);

	return name;
}

enum class StatusCode : uint8_t
{
	OK,
	READ_FAILED,
	INVALID_HEADER,
	INVALID_DOCTYPE,
};

StatusCode IsWebmFile(const std::filesystem::path& path)
{
	std::ifstream file(path, std::ios::binary);
	if(!file)
		return StatusCode::READ_FAILED;

	constexpr uint32_t READ_LIMIT = 100;
	uint8_t buf[READ_LIMIT] = {0};
	if(!file.read((char*)&buf, sizeof(buf)))
	{
		if(file.gcount() < 8)
			return StatusCode::READ_FAILED;
	}

	static const auto be32dec = [](uint8_t* p) -> uint32_t
	{
		return (uint32_t)p[3] | (uint32_t)p[2] << 1 * CHAR_BIT | (uint32_t)p[1] << 2 * CHAR_BIT | (uint32_t)p[0] << 3 * CHAR_BIT;
	};
	constexpr uint32_t EBML_HEADER_MAGIC = 0x1a45dfa3;
	uint32_t ebmlHeader = be32dec(buf);
	if(ebmlHeader != EBML_HEADER_MAGIC)
		return StatusCode::INVALID_HEADER;

	constexpr uint32_t EBML_DOCTYPE = 0x4282;
	constexpr uint8_t EBML_DOCTYPE_HIGH_BYTE = (EBML_DOCTYPE >> 1 * CHAR_BIT) & 0xff;
	constexpr uint8_t EBML_DOCTYPE_LOW_BYTE = EBML_DOCTYPE & 0xff;
	constexpr uint32_t EBML_DOCTYPE_ELEMENT_SIZE = 7;
	uint32_t docType = 0;
	for(uint32_t i = 4; i < READ_LIMIT - 1 - EBML_DOCTYPE_ELEMENT_SIZE - 1; ++i)
	{
		if(buf[i] != EBML_DOCTYPE_HIGH_BYTE)
			continue;
		if(buf[i + 1] != EBML_DOCTYPE_LOW_BYTE)
			continue;

		docType = be32dec(buf + i + 3);
		break;
	}

	constexpr auto fourcc = []<std::size_t N>(const char (&s)[N]) consteval noexcept -> uint32_t
	{
		static_assert(N == 5);
		return (uint32_t)(unsigned char)s[0] << 3 * CHAR_BIT | (uint32_t)(unsigned char)s[1] << 2 * CHAR_BIT |
			   (uint32_t)(unsigned char)s[2] << 1 * CHAR_BIT | (uint32_t)(unsigned char)s[3];
	};
	constexpr uint32_t DOCTYPE_WEBM_MAGIC = fourcc("webm");
	if(docType != DOCTYPE_WEBM_MAGIC)
		return StatusCode::INVALID_DOCTYPE;
	return StatusCode::OK;
}

} // namespace

namespace WebmAssets
{

void Scan(const std::filesystem::path& root)
{
	Clear();

	std::error_code error;
	std::filesystem::path assetRoot = std::filesystem::absolute(root, error).lexically_normal();
	if(error)
	{
		PluginLogError("Could not resolve WebM asset directory: err={} path={}", error.message(), root.string());
		return;
	}

	bool pathExists = std::filesystem::exists(assetRoot, error);
	if(error)
	{
		PluginLogError("Could not check is the directory exists: err={} path={}", error.message(), assetRoot.string());
		return;
	}
	if(!pathExists)
	{
		PluginLogWarn("WebM asset directory not exists. Creating:{}", assetRoot.string());
		std::filesystem::create_directories(assetRoot, error);
		if(error)
			PluginLogError("Could not create WebM asset directory: err={} path={}", error.message(), assetRoot.string());
	}


	auto it = std::filesystem::recursive_directory_iterator(assetRoot, error);
	if(error)
	{
		PluginLogError("Failed to scan asset directory: err={} path={}", error.message(), assetRoot.string());
		return;
	}
	for(const std::filesystem::recursive_directory_iterator end; it != end; it.increment(error))
	{
		if(error)
		{
			PluginLogWarn("Skipping WebM asset: err={} file={}", error.message(), it->path().string());
			continue;
		}

		const std::filesystem::directory_entry& entry = *it;
		bool status = entry.is_regular_file(error);
		if(error)
		{
			PluginLogWarn("Skipping WebM asset: err={} file={}", error.message(), entry.path().string());
			continue;
		}
		if(!status)
			continue;

		std::filesystem::path relativePath = entry.path().lexically_relative(assetRoot);
		std::string logicalName = GetLogicalName(relativePath);

		StatusCode statusCode = IsWebmFile(entry.path());
		if(statusCode == StatusCode::READ_FAILED)
			PluginLogWarn("Skipping WebM asset: target cannot be read: err={} file={}", "READ_FAILED", entry.path().string());
		if(statusCode == StatusCode::INVALID_HEADER)
			PluginLogWarn("Skipping WebM asset: target is not a valid EBML file: err={} file={}", "INVALID_HEADER", entry.path().string());
		if(statusCode == StatusCode::INVALID_DOCTYPE)
			PluginLogWarn("Skipping WebM asset: target may not be a WebM video: err={} file={}", "INVALID_DOCTYPE", entry.path().string());
		if(statusCode != StatusCode::OK)
			continue;

		auto [assetIt, inserted] = WebmAssetsList.try_emplace(logicalName, entry.path().lexically_normal());
		if(!inserted)
		{
			PluginLogWarn("Failed to insert WebM asset into list: file={}, duplicate={}", entry.path().string(), assetIt->second.string());
			continue;
		}
	}
	if(error)
		PluginLogWarn("Scan finished with error: {}", error.message());
	PluginLogInfo("Registered WebM assets: count={}, root={}", WebmAssetsList.size(), assetRoot.string());
}

void Clear()
{
	WebmAssetsList.clear();
}

const std::filesystem::path* Resolve(const char* requestedPath)
{
	auto asset = WebmAssetsList.find(GetLogicalName(requestedPath));
	if(asset == WebmAssetsList.end())
		return nullptr;
	return &asset->second;
}

} // namespace WebmAssets
