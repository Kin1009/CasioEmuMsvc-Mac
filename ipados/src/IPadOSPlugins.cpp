#include "IPadOSPlugins.h"
#include "PlatformTagsIPadOS.h"
#include "Plugin/PluginManifest.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace casioemu::ipados::plugins {

namespace {
std::vector<BundledPlugin> g_bundled;
std::string g_log;

void LogLine(const std::string& line) {
    g_log += "[ipados-plugins] " + line + "\n";
}
} // namespace

std::filesystem::path BundlePluginsDirectory() {
    // On iOS the bundle root is the .app directory; resources live directly in
    // it (NSBundle.mainBundle.resourcePath == .app path).
    return std::filesystem::path("Plugins");
}

const std::vector<BundledPlugin>& Bundled() { return g_bundled; }
const std::string& Log() { return g_log; }

void LoadBundledPlugins() {
    static bool loaded = false;
    if (loaded)
        return;
    loaded = true;

    std::error_code ec;
    const std::filesystem::path root = BundlePluginsDirectory();
    if (!std::filesystem::exists(root, ec)) {
        LogLine("no bundled plugin directory at " + root.string());
        return;
    }

    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const std::filesystem::path manifestPath = it->path() / "plugin.json";
        if (!std::filesystem::exists(manifestPath, ec))
            continue;

        std::ifstream file(manifestPath);
        if (!file) {
            LogLine("cannot read " + manifestPath.string());
            continue;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        const auto result = casioemu::plugin::ParseManifest(buffer.str());
        if (!result.ok) {
            LogLine(result.code + ": " + result.message + " (" + manifestPath.string() + ")");
            continue;
        }
        const auto& manifest = result.manifest;

        // Native code can never be loaded on iPadOS.
        if (manifest.IsNative()) {
            LogLine("skipped " + manifest.id + ": native_entry is not supported on iPadOS");
            continue;
        }
        // Platform tags: accept the local `ipados` proposal and the umbrella
        // `ios` tag used by older manifests.
        const bool platformOk = std::any_of(
            manifest.platforms.begin(), manifest.platforms.end(),
            [](const std::string& tag) {
                return casioemu::platform_tags::MatchesCurrentPlatform(tag);
            });
        if (!platformOk) {
            LogLine("skipped " + manifest.id + ": platform tag does not include ipados/ios");
            continue;
        }
        // Only data files may be listed in a bundled package.
        bool unsafe = false;
        for (const auto& asset : manifest.assets) {
            if (!casioemu::plugin::IsSafeRelativePath(asset)) {
                unsafe = true;
                break;
            }
        }
        if (unsafe) {
            LogLine("skipped " + manifest.id + ": unsafe asset path");
            continue;
        }

        g_bundled.push_back({manifest.id, manifest.name,
                             casioemu::plugin::TypeName(manifest.type), it->path()});
        LogLine("registered " + manifest.id + " (" + g_bundled.back().type + ")");
    }
}

} // namespace casioemu::ipados::plugins
