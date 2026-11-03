#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace casioemu::plugin {

// A validated injection payload contributed by an `inject` plugin.  The
// Injection/Payload window renders these next to the user's own file-backed
// injections; the host registry lives here so the loader and the UI share one
// source of truth without a GUI dependency in the loader.
struct InjectionPayload {
	std::string pluginId;
	std::string id;
	std::string name;
	std::string description;
	std::string target = "any";
	std::string addressText;
	uint32_t address = 0;
	std::vector<uint8_t> bytes;
	bool safe = false;
};

// Parses the `payloads` array of an inject document.  On failure returns false
// and fills `error` with a human-readable, payload-specific message.  A single
// invalid payload rejects the whole document: the plugin must not report
// "Loaded" while silently dropping data.
bool ParseInjectDocument(const std::string& jsonText, const std::string& pluginId,
	std::vector<InjectionPayload>& out, std::string& error);

void RegisterPluginPayloads(const std::string& pluginId, std::vector<InjectionPayload> payloads);
void ClearPluginPayloads(const std::string& pluginId);
[[nodiscard]] std::vector<InjectionPayload> PluginPayloads();
[[nodiscard]] bool FindPluginPayload(const std::string& id, InjectionPayload& out);

// Applies payload bytes through the MMU.  Declared in the header so the
// Injection window (and tests) can share the exact write path.  `write` is
// supplied by the caller to keep this module free of emulator headers.
bool ApplyPayloadBytes(const InjectionPayload& payload,
	bool (*write)(void* context, uint32_t address, uint8_t byte), void* context, std::string& error);

} // namespace casioemu::plugin
