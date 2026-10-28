#pragma once

#include <string>

namespace casioemu::plugin {

// Apply a theme plugin document to the current ImGui style.
// Returns false and fills `error` when the document is malformed.
bool ApplyThemeDocument(const std::string& jsonText, std::string& error);

} // namespace casioemu::plugin
