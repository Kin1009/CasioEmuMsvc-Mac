#pragma once

#include "PluginManifest.h"

#include <filesystem>
#include <string>

namespace casioemu::plugin {

struct Package {
	std::filesystem::path file;
	std::filesystem::path root;
	std::string sha256;
	Manifest manifest;
	bool valid = false;
	std::string code;
	std::string message;
};

bool ComputeFileSha256(const std::filesystem::path& file, std::string& digest, std::string& error);
bool ExtractPackage(const std::filesystem::path& file, const std::filesystem::path& cacheRoot, Package& out);

} // namespace casioemu::plugin
