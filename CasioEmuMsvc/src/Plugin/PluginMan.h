#pragma once
#include "IArchitectureProvider.h"
#include <string>
#include <vector>

class PluginApi;  // forward declaration: keep this header safe to include from host TUs

// Global architecture-provider registry (owned by PluginMan.cpp).
casioemu::IArchitectureRegistry* GetArchitectureRegistry();

struct PluginInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string desc;
};

extern std::string g_PluginLoadLog;
extern std::vector<PluginInfo> g_loadedPlugins;
void LoadPlugins();

/// Early, architecture-only plugin pass. Loads plugin modules and calls their
/// OPTIONAL `fPluginLoadArch` export. Must run before Emulator construction.
void LoadArchitecturePlugins();
