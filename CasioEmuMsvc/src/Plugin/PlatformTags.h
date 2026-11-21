#pragma once

// ---------------------------------------------------------------------------
// PlatformTags.h -- OS / CPU-architecture tags for multi-platform .plugin files
// ---------------------------------------------------------------------------
//
// A `.plugin` ZIP may ship raw native binaries for several OS/architecture
// combinations.  Files follow the pattern:
//
//     <name>-<os>-<arch>.<ext>
//
// Examples:
//     my-plugin1-macos-x64.dylib
//     my-plugin2-macos-arm64.dylib
//     my-plugin1-windows-x64.dll
//     my-plugin1-linux-arm64.so
//     my-plugin1-android-arm64-android.so
//     my-plugin-any-any.so            (wildcard OS + wildcard architecture)
//
// Multiple binaries per platform are allowed: every binary whose <os> and
// <arch> match the running host is loaded.  If a package contains platform
// tagged binaries but NONE match the host, the plugin is rejected with the
// message produced by NoCompatibleBinaryError(), e.g.
//     "No native binary for macos-x64"
//
// Tags (kept deliberately small and stable):
//   OS   : macos | linux | windows | android | any
//   ARCH : x64 | x32 | arm64 | arm64-android | any
//
//   * x64            -> 64-bit x86 (AMD64 / Intel64), desktop/laptop
//   * x32            -> 32-bit x86 (IA-32)
//   * arm64          -> 64-bit ARM on laptops/desktops (Apple Silicon,
//                       Windows on ARM, Linux aarch64)
//   * arm64-android  -> 64-bit ARM on Android phones/tablets
//   * any            -> wildcard, matches every build
//
// The current build's OS/arch are exposed as CASIOEMU_PLATFORM_OS and
// CASIOEMU_PLATFORM_ARCH.  A build system may override them with
// `target_compile_definitions(... CASIOEMU_PLATFORM_OS="macos" ...)`;
// otherwise this header derives them from the compiler's own predefines so it
// is correct on every host without any build-system changes.
//
// The host owns the ABI function names (see ArchCoreABI.h); a plugin merely
// implements them.  This header only decides WHICH binaries to hand to dlopen.

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace casioemu::plugin {

// ---------------------------------------------------------------------------
// Current build platform.  The compiler's own target predefines are
// authoritative.  Some build configurations inject these names as
// over-escaped string macros (e.g. -DCASIOEMU_PLATFORM_OS=\\\"macos\\\"),
// which are invalid C++ tokens; discard any such injected value and derive
// the tags from the actual compilation target instead.
// ---------------------------------------------------------------------------
#ifdef CASIOEMU_PLATFORM_OS
#undef CASIOEMU_PLATFORM_OS
#endif
#ifdef CASIOEMU_PLATFORM_ARCH
#undef CASIOEMU_PLATFORM_ARCH
#endif

#ifndef CASIOEMU_PLATFORM_OS
#if defined(__ANDROID__)
#define CASIOEMU_PLATFORM_OS "android"
#elif defined(_WIN32)
#define CASIOEMU_PLATFORM_OS "windows"
#elif defined(__APPLE__)
#define CASIOEMU_PLATFORM_OS "macos"
#elif defined(__linux__)
#define CASIOEMU_PLATFORM_OS "linux"
#else
#define CASIOEMU_PLATFORM_OS "any"
#endif
#endif

#ifndef CASIOEMU_PLATFORM_ARCH
#if defined(__ANDROID__)
#if defined(__aarch64__) || defined(_M_ARM64)
#define CASIOEMU_PLATFORM_ARCH "arm64-android"
#else
// No dedicated 32-bit-ARM-Android tag in the format; treat as wildcard so a
// package can still supply an "any" binary for such a device.
#define CASIOEMU_PLATFORM_ARCH "any"
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#define CASIOEMU_PLATFORM_ARCH "arm64"
#elif defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
#define CASIOEMU_PLATFORM_ARCH "x64"
#elif defined(__i386__) || defined(_M_IX86)
#define CASIOEMU_PLATFORM_ARCH "x32"
#else
#define CASIOEMU_PLATFORM_ARCH "any"
#endif
#endif

// ---------------------------------------------------------------------------
// Known tags and helper constants.
// ---------------------------------------------------------------------------
inline constexpr std::string_view kKnownOsTags[] = {
	"macos", "linux", "windows", "android", "any",
};
inline constexpr std::string_view kKnownArchTags[] = {
	"x64", "x32", "arm64", "arm64-android", "any",
};

inline constexpr std::string_view kOsMacos = "macos";
inline constexpr std::string_view kOsLinux = "linux";
inline constexpr std::string_view kOsWindows = "windows";
inline constexpr std::string_view kOsAndroid = "android";
inline constexpr std::string_view kOsAny = "any";

inline constexpr std::string_view kArchX64 = "x64";
inline constexpr std::string_view kArchX32 = "x32";
inline constexpr std::string_view kArchArm64 = "arm64";
inline constexpr std::string_view kArchArm64Android = "arm64-android";
inline constexpr std::string_view kArchAny = "any";

inline bool IsKnownOsTag(std::string_view tag) {
	for (std::string_view known : kKnownOsTags)
		if (known == tag)
			return true;
	return false;
}

inline bool IsKnownArchTag(std::string_view tag) {
	for (std::string_view known : kKnownArchTags)
		if (known == tag)
			return true;
	return false;
}

inline const char* OsTagDescription(std::string_view tag) {
	if (tag == "macos") return "Apple macOS (desktop / laptop)";
	if (tag == "linux") return "Linux";
	if (tag == "windows") return "Microsoft Windows";
	if (tag == "android") return "Android";
	if (tag == "any") return "Any operating system (wildcard)";
	return "Unknown OS tag";
}

inline const char* ArchTagDescription(std::string_view tag) {
	if (tag == "x64") return "64-bit x86 (AMD64 / Intel64)";
	if (tag == "x32") return "32-bit x86 (IA-32)";
	if (tag == "arm64") return "64-bit ARM on laptops / desktops (Apple Silicon, Windows ARM, Linux aarch64)";
	if (tag == "arm64-android") return "64-bit ARM on Android phones / tablets";
	if (tag == "any") return "Any CPU architecture (wildcard)";
	return "Unknown architecture tag";
}

// ---------------------------------------------------------------------------
// Matching a file's tags against a build.  "any" is a wildcard on either side.
// ---------------------------------------------------------------------------
inline bool OsTagMatches(std::string_view tag, std::string_view buildOs = CASIOEMU_PLATFORM_OS) {
	if (tag == "any" || buildOs == "any")
		return true;
	return tag == buildOs;
}

inline bool ArchTagMatches(std::string_view tag, std::string_view buildArch = CASIOEMU_PLATFORM_ARCH) {
	if (tag == "any" || buildArch == "any")
		return true;
	return tag == buildArch;
}

inline bool PlatformMatchesBuildImpl(std::string_view os, std::string_view arch,
	std::string_view buildOs, std::string_view buildArch) {
	return OsTagMatches(os, buildOs) && ArchTagMatches(arch, buildArch);
}

inline bool PlatformMatchesBuild(std::string_view os, std::string_view arch) {
	return PlatformMatchesBuildImpl(os, arch, CASIOEMU_PLATFORM_OS, CASIOEMU_PLATFORM_ARCH);
}

// "macos-x64", "windows-arm64", ... -- used in logs and error messages.
inline std::string MakePlatformKey(std::string_view os, std::string_view arch) {
	return std::string(os) + "-" + std::string(arch);
}

inline std::string CurrentPlatformKey() {
	return MakePlatformKey(CASIOEMU_PLATFORM_OS, CASIOEMU_PLATFORM_ARCH);
}

// Clear, host-independent mismatch message.  Shared by the registry, the
// early architecture pass and the verification harness so the exact same text
// is always reported.
inline std::string NoCompatibleBinaryError(
	std::string_view os = CASIOEMU_PLATFORM_OS,
	std::string_view arch = CASIOEMU_PLATFORM_ARCH) {
	return "No native binary for " + MakePlatformKey(os, arch);
}

// ---------------------------------------------------------------------------
// Tagged filename parsing.  Pattern: <name>-<os>-<arch>.<ext>
// The arch tag may itself contain a hyphen ("arm64-android"), so tags are
// matched as known suffixes rather than by splitting on '-'.
// ---------------------------------------------------------------------------
struct TaggedNativeFile {
	std::string name;  // base name before the -os-arch suffix
	std::string os;
	std::string arch;
	std::string ext;   // file extension, including the leading '.' (e.g. ".dylib")
};

inline bool IsNativeLibraryExtension(std::string_view ext) {
	return ext == ".dylib" || ext == ".so" || ext == ".dll" || ext == ".plugin";
}

inline std::optional<TaggedNativeFile> ParseTaggedNativeFileName(std::string_view fileName) {
	const size_t dot = fileName.rfind('.');
	if (dot == std::string_view::npos || dot == 0 || dot + 1 >= fileName.size())
		return std::nullopt;
	const std::string_view stem = fileName.substr(0, dot);
	const std::string extension(fileName.substr(dot));

	for (std::string_view os : kKnownOsTags) {
		for (std::string_view arch : kKnownArchTags) {
			std::string suffix = "-";
			suffix += os;
			suffix += '-';
			suffix += arch;
			if (stem.size() > suffix.size() &&
				stem.substr(stem.size() - suffix.size()) == suffix) {
				return TaggedNativeFile{
					std::string(stem.substr(0, stem.size() - suffix.size())),
					std::string(os),
					std::string(arch),
					extension,
				};
			}
		}
	}
	return std::nullopt;
}

// ---------------------------------------------------------------------------
// Scanning an extracted package and selecting this host's binaries.
// ---------------------------------------------------------------------------
struct PlatformBinarySelection {
	enum class Status {
		NoTaggedBinaries,  // no platform-tagged files: use legacy native_entry
		Matched,           // one or more tagged files match this host
		NoMatch,           // tagged files exist, but none match -> error
	};

	Status status = Status::NoTaggedBinaries;
	// Absolute paths of every matching native binary (multiple per platform
	// are allowed; the caller loads all of them).
	std::vector<std::filesystem::path> matched;
	// "<os>-<arch>" keys of all tagged binaries, for diagnostics.
	std::vector<std::string> available;
};

inline PlatformBinarySelection SelectPlatformBinariesForRoot(
	const std::filesystem::path& root,
	std::string_view buildOs = CASIOEMU_PLATFORM_OS,
	std::string_view buildArch = CASIOEMU_PLATFORM_ARCH) {
	PlatformBinarySelection selection;
	std::error_code ec;
	if (!std::filesystem::is_directory(root, ec))
		return selection;

	for (auto& item : std::filesystem::recursive_directory_iterator(root, ec)) {
		if (ec)
			break;
		std::error_code file_ec;
		if (!item.is_regular_file(file_ec))
			continue;
		const auto parsed = ParseTaggedNativeFileName(item.path().filename().string());
		if (!parsed || !IsNativeLibraryExtension(parsed->ext))
			continue;
		const std::string key = MakePlatformKey(parsed->os, parsed->arch);
		if (std::find(selection.available.begin(), selection.available.end(), key) == selection.available.end())
			selection.available.push_back(key);
		if (PlatformMatchesBuildImpl(parsed->os, parsed->arch, buildOs, buildArch))
			selection.matched.push_back(item.path());
	}

	std::sort(selection.matched.begin(), selection.matched.end());
	std::sort(selection.available.begin(), selection.available.end());

	if (selection.available.empty())
		selection.status = PlatformBinarySelection::Status::NoTaggedBinaries;
	else if (selection.matched.empty())
		selection.status = PlatformBinarySelection::Status::NoMatch;
	else
		selection.status = PlatformBinarySelection::Status::Matched;
	return selection;
}

} // namespace casioemu::plugin
