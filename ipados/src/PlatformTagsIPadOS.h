#pragma once
// LOCAL MIRROR of the proposed `ipados` platform tag.
//
// The canonical `PlatformTags.h` is owned by another agent in the main tree;
// this worktree must not edit it.  Once the canonical header exposes the tag,
// delete this file and replace the include in IPadOSPlugins.cpp with
// `#include "PlatformTags.h"`.
//
// Proposal for the canonical header:
//   inline constexpr const char* kPlatformIPadOS = "ipados";
//   // `ios` remains the umbrella tag; `ipados` is the device-family-specific
//   // tag used by plugin manifests so a package can target iPad only.
//   // Manifest matching must treat `ios` as matching `ipados` and vice versa
//   // for backwards compatibility with existing `["macos"]` manifests.

#include <string_view>

namespace casioemu::platform_tags {
inline constexpr std::string_view kIPadOS = "ipados";
inline constexpr std::string_view kIOS = "ios";

[[nodiscard]] inline bool MatchesCurrentPlatform(std::string_view tag) {
	return tag == kIPadOS || tag == kIOS;
}
} // namespace casioemu::platform_tags
