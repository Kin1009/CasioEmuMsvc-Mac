// Verifies that PlatformTags.h selects the Windows x64 native binary from an
// extracted .plugin package when compiled as a Windows target.
#include "PlatformTags.h"

#include <cstdio>
#include <filesystem>

int main(int argc, char** argv) {
	if (argc < 2) {
		std::printf("usage: verify_platform_tags.exe <extracted-plugin-dir>\n");
		return 2;
	}
	namespace tags = casioemu::plugin;
	std::printf("current platform: %s\n", tags::CurrentPlatformKey().c_str());
	const auto selection = tags::SelectPlatformBinariesForRoot(argv[1]);
	std::printf("status: %d (%s)\n", static_cast<int>(selection.status),
		selection.status == tags::PlatformBinarySelection::Status::Matched ? "Matched"
		: selection.status == tags::PlatformBinarySelection::Status::NoMatch ? "NoMatch"
		: "NoTaggedBinaries");
	std::printf("available:");
	for (const auto& key : selection.available)
		std::printf(" %s", key.c_str());
	std::printf("\n");
	for (const auto& path : selection.matched)
		std::printf("matched: %s\n", path.filename().string().c_str());
	const bool ok = selection.status == tags::PlatformBinarySelection::Status::Matched &&
		selection.matched.size() == 1 &&
		selection.matched[0].filename() == "CG100SH4-windows-x64.dll";
	std::printf(ok ? "=== PLATFORM TAG SELECTION PASSED ===\n"
		: "=== PLATFORM TAG SELECTION FAILED ===\n");
	return ok ? 0 : 1;
}
