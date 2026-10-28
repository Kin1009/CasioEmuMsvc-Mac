#include "PluginPackage.h"

#include "miniz.h"

#include <CommonCrypto/CommonDigest.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace casioemu::plugin {
namespace {

void SetError(Package& out, const std::string& code, const std::string& message) {
	out.valid = false;
	out.code = code;
	out.message = message;
}

std::string HexEncode(const unsigned char* data, size_t size) {
	static constexpr char kDigits[] = "0123456789abcdef";
	std::string output;
	output.resize(size * 2);
	for (size_t i = 0; i < size; ++i) {
		output[i * 2] = kDigits[data[i] >> 4];
		output[i * 2 + 1] = kDigits[data[i] & 0xF];
	}
	return output;
}

} // namespace

bool ComputeFileSha256(const std::filesystem::path& file, std::string& digest, std::string& error) {
	std::ifstream input(file, std::ios::binary);
	if (!input) {
		error = "cannot open package for hashing";
		return false;
	}

	CC_SHA256_CTX context;
	CC_SHA256_Init(&context);
	std::array<char, 64 * 1024> buffer{};
	while (input.good()) {
		input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
		const std::streamsize read = input.gcount();
		if (read > 0)
			CC_SHA256_Update(&context, buffer.data(), static_cast<CC_LONG>(read));
	}
	if (input.bad()) {
		error = "failed while reading package";
		return false;
	}

	unsigned char hash[CC_SHA256_DIGEST_LENGTH]{};
	CC_SHA256_Final(hash, &context);
	digest = HexEncode(hash, sizeof(hash));
	return true;
}

bool ExtractPackage(const std::filesystem::path& file, const std::filesystem::path& cacheRoot, Package& out) {
	out = {};
	out.file = file;

	std::string hash_error;
	if (!ComputeFileSha256(file, out.sha256, hash_error)) {
		SetError(out, "package.sha256", hash_error);
		return false;
	}

	const std::filesystem::path cache_dir = cacheRoot / out.sha256;
	std::error_code ec;
	if (std::filesystem::exists(cache_dir / "plugin.json", ec)) {
		out.root = cache_dir;
		ManifestResult manifest = LoadManifestFile(cache_dir / "plugin.json");
		out.code = manifest.code;
		out.message = manifest.message;
		out.manifest = std::move(manifest.manifest);
		out.valid = manifest.ok;
		return out.valid;
	}

	const std::filesystem::path temp_dir = cache_dir.string() + ".tmp-" + std::to_string(::getpid());
	std::filesystem::remove_all(temp_dir, ec);
	std::filesystem::create_directories(temp_dir, ec);
	if (ec) {
		SetError(out, "package.path", "cannot create plugin cache: " + ec.message());
		return false;
	}

	mz_zip_archive archive{};
	if (!mz_zip_reader_init_file(&archive, file.string().c_str(), 0)) {
		std::filesystem::remove_all(temp_dir, ec);
		SetError(out, "package.invalid_zip", "the file is not a readable zip archive");
		return false;
	}

	bool ok = true;
	const mz_uint file_count = mz_zip_reader_get_num_files(&archive);
	for (mz_uint index = 0; index < file_count && ok; ++index) {
		mz_zip_archive_file_stat stat{};
		if (!mz_zip_reader_file_stat(&archive, index, &stat)) {
			SetError(out, "package.invalid_zip", "cannot read a zip entry");
			ok = false;
			break;
		}

		const std::string name = stat.m_filename;
		if (!IsSafeRelativePath(name)) {
			SetError(out, "package.path", "unsafe zip entry: " + name);
			ok = false;
			break;
		}

		const uint32_t mode = static_cast<uint32_t>(stat.m_external_attr >> 16);
		if (mode != 0 && S_ISLNK(mode)) {
			SetError(out, "package.path", "symlink entries are not allowed: " + name);
			ok = false;
			break;
		}

		const std::filesystem::path destination = temp_dir / name;
		if (mz_zip_reader_is_file_a_directory(&archive, index) || stat.m_is_directory) {
			std::filesystem::create_directories(destination, ec);
			if (ec) {
				SetError(out, "package.path", "cannot create directory: " + ec.message());
				ok = false;
			}
			continue;
		}

		std::filesystem::create_directories(destination.parent_path(), ec);
		if (ec) {
			SetError(out, "package.path", "cannot create directory: " + ec.message());
			ok = false;
			break;
		}

		if (!mz_zip_reader_extract_to_file(&archive, index, destination.string().c_str(), 0)) {
			SetError(out, "package.invalid_zip", "cannot extract: " + name);
			ok = false;
			break;
		}
	}
	mz_zip_reader_end(&archive);

	if (!ok) {
		std::filesystem::remove_all(temp_dir, ec);
		return false;
	}

	ManifestResult manifest = LoadManifestFile(temp_dir / "plugin.json");
	if (!manifest.ok) {
		std::filesystem::remove_all(temp_dir, ec);
		out.code = manifest.code;
		out.message = manifest.message;
		out.valid = false;
		return false;
	}

	std::filesystem::remove_all(cache_dir, ec);
	std::filesystem::rename(temp_dir, cache_dir, ec);
	if (ec) {
		std::filesystem::remove_all(temp_dir, ec);
		SetError(out, "package.path", "cannot commit plugin cache: " + ec.message());
		return false;
	}

	out.root = cache_dir;
	out.manifest = std::move(manifest.manifest);
	out.valid = true;
	return true;
}

} // namespace casioemu::plugin
