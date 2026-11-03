#pragma once

namespace casioemu::plugin {

// The single source of truth for the host/plugin ABI version.  PluginApi.h
// exposes it as PLUGIN_API_VERSION; PluginManifest.h as kHostApiVersion.
// Bump both the revision note here and kPluginUiWindowAbiRevision when the
// UIWindow layout or an existing virtual interface changes.
inline constexpr int kPluginHostApiVersion = 3;

// Oldest manifest api_version a v3 host still accepts.
inline constexpr int kPluginHostApiMinVersion = 1;

} // namespace casioemu::plugin
