#pragma once
// Declarative (data-only) plugin bootstrap for iPadOS.
//
// iPadOS forbids dlopen of code that is not part of the app bundle and signed
// by the developer, and forbids JIT.  Native .plugin packages are therefore
// not supported.  Instead, plugins that consist only of plugin.json + JSON
// assets are shipped inside the app bundle at build time and registered here.

#include <filesystem>
#include <string>
#include <vector>

namespace casioemu::ipados::plugins {

struct BundledPlugin {
    std::string id;
    std::string name;
    std::string type;
    std::filesystem::path root;
};

/// Scan <app bundle>/Resources/Plugins for data-only packages.
/// Results are appended to the process-global list returned by Bundled().
void LoadBundledPlugins();

[[nodiscard]] const std::vector<BundledPlugin>& Bundled();
[[nodiscard]] const std::string& Log();

/// Directory used for bundled packages (relative to the bundle root).
[[nodiscard]] std::filesystem::path BundlePluginsDirectory();

} // namespace casioemu::ipados::plugins
