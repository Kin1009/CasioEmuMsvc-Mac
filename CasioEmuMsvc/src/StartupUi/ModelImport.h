#pragma once
// Model package import/export used by the launcher (StartupUi).
//
// One implementation is shared by the Import... button, the encrypted-password
// flow and the headless import self-test so a fixture exercises exactly the
// code path the UI uses.

#include "Ext/RomPackage.h"
#include "ModelConfig.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace casioemu::startup {

struct ModelImportResult {
	bool ok = false;
	bool encrypted = false;
	bool needsPassword = false;
	std::string error;
	std::string modelName;
	std::string targetPath;
	std::vector<std::string> files;
};

inline std::filesystem::path DefaultModelsDirectory() {
	if (const char* override = std::getenv("CASIOEMU_MODELS_DIR"))
		return std::filesystem::path(override);
	std::error_code ec;
	const auto current = std::filesystem::current_path(ec);
	if (ec)
		return std::filesystem::path("models");
	return current / "models";
}

inline std::string SanitizeModelFolder(const std::string& input) {
	std::string result;
	for (unsigned char c : input) {
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
			c == '_' || c == '-' || c == '.' || c == ' ')
			result += static_cast<char>(c);
		else if (c >= 32 && c < 127)
			result += '_';
	}
	const size_t start = result.find_first_not_of(" .");
	const size_t end = result.find_last_not_of(" .");
	if (start == std::string::npos)
		return "imported-model";
	result = result.substr(start, end - start + 1);
	while (result.find("__") != std::string::npos)
		result.erase(result.find("__"), 1);
	if (result.size() > 96)
		result.resize(96);
	return result.empty() ? "imported-model" : result;
}

inline std::filesystem::path UniqueModelDirectory(const std::filesystem::path& models_dir,
		const std::string& base_name) {
	const std::string safe = SanitizeModelFolder(base_name);
	std::filesystem::path candidate = models_dir / safe;
	if (!std::filesystem::exists(candidate))
		return candidate;
	for (int suffix = 2; suffix < 10000; ++suffix) {
		candidate = models_dir / (safe + "." + std::to_string(suffix));
		if (!std::filesystem::exists(candidate))
			return candidate;
	}
	return models_dir / (safe + ".imported");
}

inline ModelImportResult ImportModelPackage(const std::filesystem::path& package_path,
		const std::filesystem::path& models_dir, const std::string& password = std::string()) {
	ModelImportResult result;
	std::error_code ec;
	if (!std::filesystem::exists(package_path, ec) || !std::filesystem::is_regular_file(package_path, ec)) {
		result.error = "The selected file does not exist.";
		return result;
	}
	RomPackage package;
	{
		std::ifstream input(package_path, std::ios::binary);
		if (!input) {
			result.error = "Cannot open " + package_path.string();
			return result;
		}
		try {
			Binary::Read(input, package);
		}
		catch (const std::exception& error) {
			result.error = std::string("Invalid or truncated model package: ") + error.what();
			return result;
		}
		if (input.bad()) {
			result.error = "The package could not be read completely (truncated file).";
			return result;
		}
	}
	result.encrypted = package.IsEncrypted || package.EncryptionVersion != 0;
	if (result.encrypted) {
		if (password.empty()) {
			result.needsPassword = true;
			result.error = "This package is encrypted. Enter its password to import it.";
			return result;
		}
		try {
			package.Decrypt(password);
		}
		catch (const std::exception& error) {
			result.error = std::string("Wrong password or damaged package: ") + error.what();
			return result;
		}
	}
	if (package.ModelInfo.rom_path.empty()) {
		result.error = "The package does not describe a ROM file (rom_path is empty).";
		return result;
	}
	if (package.ModelInfo.interface_path.empty()) {
		result.error = "The package does not describe an interface image (interface_path is empty).";
		return result;
	}
	result.modelName = package.ModelInfo.model_name.empty()
		? package_path.stem().string() : package.ModelInfo.model_name;
	if (!std::filesystem::create_directories(models_dir, ec) && ec) {
		result.error = "Cannot create the models folder: " + ec.message();
		return result;
	}
	const std::filesystem::path target = UniqueModelDirectory(models_dir, result.modelName);
	auto path_is_safe = [](const std::string& value) {
		if (value.empty())
			return true;
		const std::filesystem::path candidate(value);
		if (candidate.is_absolute())
			return false;
		for (const auto& part : candidate) {
			if (part == "..")
				return false;
		}
		return true;
	};
	if (!path_is_safe(package.ModelInfo.rom_path) ||
		!path_is_safe(package.ModelInfo.interface_path) ||
		(!package.ModelInfo.flash_path.empty() && !path_is_safe(package.ModelInfo.flash_path))) {
		result.error = "The package contains an unsafe file path.";
		return result;
	}
	try {
		// Pre-create nested payload folders (some packages keep assets in a
		// subdirectory) so ExtractTo's single create_directory() cannot fail.
		std::filesystem::create_directories(target);
		std::filesystem::create_directories((target / package.ModelInfo.rom_path).parent_path());
		std::filesystem::create_directories((target / package.ModelInfo.interface_path).parent_path());
		if (!package.ModelInfo.flash_path.empty())
			std::filesystem::create_directories((target / package.ModelInfo.flash_path).parent_path());
		package.ExtractTo(target);
	}
	catch (const std::exception& error) {
		std::filesystem::remove_all(target, ec);
		result.error = std::string("Extraction failed: ") + error.what();
		return result;
	}
	// Verify the package registers exactly like a model that has always been on
	// disk.  A package with a broken config must not leave a half-model behind.
	ModelInfo registered{};
	std::string register_error;
	if (!LoadModelInfoFromFolder(target, registered, nullptr, &register_error)) {
		std::filesystem::remove_all(target, ec);
		result.error = "The package was extracted but its configuration is invalid: " + register_error;
		return result;
	}
	const std::filesystem::path rom_file = target / registered.rom_path;
	const std::filesystem::path interface_file = target / registered.interface_path;
	if (!std::filesystem::is_regular_file(rom_file, ec) || std::filesystem::file_size(rom_file, ec) == 0) {
		std::filesystem::remove_all(target, ec);
		result.error = "The package is missing its ROM payload (" + registered.rom_path + ").";
		return result;
	}
	if (!std::filesystem::is_regular_file(interface_file, ec)) {
		std::filesystem::remove_all(target, ec);
		result.error = "The package is missing its interface image (" + registered.interface_path + ").";
		return result;
	}
	result.ok = true;
	result.modelName = registered.model_name.empty() ? result.modelName : registered.model_name;
	result.targetPath = target.string();
	for (const auto& entry : std::filesystem::directory_iterator(target, ec)) {
		if (entry.is_regular_file())
			result.files.push_back(entry.path().filename().string());
	}
	std::sort(result.files.begin(), result.files.end());
	return result;
}

// Used by the headless import self-test to build a fixture package from a
// folder with the same serializer the Export menu uses.
inline bool WriteModelPackage(const std::filesystem::path& model_folder,
		const std::filesystem::path& package_path, std::string* error) {
	try {
		RomPackage package;
		package.Load(model_folder);
		std::filesystem::create_directories(package_path.parent_path());
		std::ofstream output(package_path, std::ios::binary | std::ios::trunc);
		if (!output)
			throw std::runtime_error("cannot create " + package_path.string());
		Binary::Write(output, package);
		return true;
	}
	catch (const std::exception& exception) {
		if (error)
			*error = exception.what();
		return false;
	}
}

} // namespace casioemu::startup
