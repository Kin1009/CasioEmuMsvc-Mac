#include "PluginPayloads.h"

#include "../../../McpPlugin/json.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>
#include <sstream>

namespace casioemu::plugin {
namespace {

using json = nlohmann::json;

constexpr size_t kMaxPayloadBytes = 4096;
constexpr size_t kMaxPayloadsPerPlugin = 128;

std::mutex g_payload_mutex;
std::vector<InjectionPayload> g_payloads;

std::string ToLower(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

bool IsValidPayloadId(const std::string& id) {
	if (id.empty() || id.size() > 96)
		return false;
	for (char c : id) {
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			return false;
	}
	return true;
}

int HexDigit(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

bool ParseAddress(const std::string& text, uint32_t& address, std::string& error) {
	std::string value = text;
	if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
		value = value.substr(2);
	if (value.empty() || value.size() > 8) {
		error = "address must be 1-8 hex digits (optionally prefixed with 0x)";
		return false;
	}
	uint32_t parsed = 0;
	for (char c : value) {
		const int digit = HexDigit(c);
		if (digit < 0) {
			error = "address is not hexadecimal: '" + text + "'";
			return false;
		}
		parsed = (parsed << 4) | static_cast<uint32_t>(digit);
	}
	address = parsed;
	return true;
}

bool ParseBytes(const std::string& text, std::vector<uint8_t>& bytes, std::string& error) {
	std::string hex;
	for (char c : text) {
		if (std::isspace(static_cast<unsigned char>(c)))
			continue;
		if (HexDigit(c) < 0) {
			error = "bytes must be hexadecimal (spaces allowed)";
			return false;
		}
		hex.push_back(c);
	}
	if (hex.empty() || (hex.size() % 2) != 0) {
		error = "bytes must contain an even, non-zero number of hex digits";
		return false;
	}
	if (hex.size() / 2 > kMaxPayloadBytes) {
		error = "payload is larger than " + std::to_string(kMaxPayloadBytes) + " bytes";
		return false;
	}
	for (size_t i = 0; i < hex.size(); i += 2)
		bytes.push_back(static_cast<uint8_t>((HexDigit(hex[i]) << 4) | HexDigit(hex[i + 1])));
	return true;
}

std::string OptionalString(const json& item, const char* key, std::string& error) {
	auto it = item.find(key);
	if (it == item.end() || it->is_null())
		return {};
	if (!it->is_string()) {
		error = std::string("field '") + key + "' must be a string";
		return {};
	}
	return it->get<std::string>();
}

} // namespace

bool ParseInjectDocument(const std::string& jsonText, const std::string& pluginId,
	std::vector<InjectionPayload>& out, std::string& error) {
	out.clear();
	json document;
	try {
		document = json::parse(jsonText);
	}
	catch (const json::exception& exception) {
		error = std::string("inject document is not valid JSON: ") + exception.what();
		return false;
	}
	if (!document.is_object()) {
		error = "inject document must be a JSON object";
		return false;
	}
	if (document.value("schema", 0) != 1) {
		error = "inject document must have schema 1";
		return false;
	}
	auto payloads = document.find("payloads");
	if (payloads == document.end() || !payloads->is_array()) {
		error = "inject plugin is missing the 'payloads' array";
		return false;
	}
	if (payloads->empty()) {
		error = "inject plugin must declare at least one payload";
		return false;
	}
	if (payloads->size() > kMaxPayloadsPerPlugin) {
		error = "inject plugin declares more than " + std::to_string(kMaxPayloadsPerPlugin) + " payloads";
		return false;
	}

	std::set<std::string> seen_ids;
	size_t index = 0;
	for (const auto& item : *payloads) {
		const std::string prefix = "payloads[" + std::to_string(index) + "]";
		++index;
		if (!item.is_object()) {
			error = prefix + " must be an object";
			return false;
		}

		InjectionPayload payload;
		payload.pluginId = pluginId;

		std::string field_error;
		payload.id = OptionalString(item, "id", field_error);
		if (!field_error.empty()) {
			error = prefix + ".id: " + field_error;
			return false;
		}
		if (payload.id.empty()) {
			error = prefix + " is missing 'id'";
			return false;
		}
		if (!IsValidPayloadId(payload.id)) {
			error = prefix + ".id: must be 1-96 characters of a-z, 0-9, '.', '_' or '-'";
			return false;
		}
		if (!seen_ids.insert(payload.id).second) {
			error = prefix + ".id: duplicate payload id '" + payload.id + "'";
			return false;
		}

		payload.name = OptionalString(item, "name", field_error);
		if (!field_error.empty()) {
			error = prefix + ".name: " + field_error;
			return false;
		}
		if (payload.name.empty() || payload.name.size() > 80) {
			error = prefix + ".name must be 1-80 characters";
			return false;
		}

		payload.description = OptionalString(item, "description", field_error);
		if (!field_error.empty()) {
			error = prefix + ".description: " + field_error;
			return false;
		}
		if (payload.description.size() > 500) {
			error = prefix + ".description must be at most 500 characters";
			return false;
		}

		payload.target = OptionalString(item, "target", field_error);
		if (!field_error.empty()) {
			error = prefix + ".target: " + field_error;
			return false;
		}
		if (payload.target.empty())
			payload.target = "any";
		const std::string target = ToLower(payload.target);
		if (target != "any" && target != "t4x" && target != "eps" && target != "legacy") {
			error = prefix + ".target: unsupported target '" + payload.target + "' (any, t4x, eps, legacy)";
			return false;
		}

		payload.addressText = OptionalString(item, "address", field_error);
		if (!field_error.empty()) {
			error = prefix + ".address: " + field_error;
			return false;
		}
		if (payload.addressText.empty()) {
			error = prefix + " is missing 'address'";
			return false;
		}
		std::string address_error;
		if (!ParseAddress(payload.addressText, payload.address, address_error)) {
			error = prefix + ".address: " + address_error;
			return false;
		}

		const std::string bytes = OptionalString(item, "bytes", field_error);
		if (!field_error.empty()) {
			error = prefix + ".bytes: " + field_error;
			return false;
		}
		if (bytes.empty()) {
			error = prefix + " is missing 'bytes'";
			return false;
		}
		std::string bytes_error;
		if (!ParseBytes(bytes, payload.bytes, bytes_error)) {
			error = prefix + ".bytes: " + bytes_error;
			return false;
		}

		auto safe = item.find("safe");
		if (safe != item.end() && !safe->is_null()) {
			if (!safe->is_boolean()) {
				error = prefix + ".safe must be a boolean";
				return false;
			}
			payload.safe = safe->get<bool>();
		}

		out.push_back(std::move(payload));
	}
	return true;
}

void RegisterPluginPayloads(const std::string& pluginId, std::vector<InjectionPayload> payloads) {
	std::lock_guard<std::mutex> lock(g_payload_mutex);
	g_payloads.erase(std::remove_if(g_payloads.begin(), g_payloads.end(),
		[&](const InjectionPayload& payload) { return payload.pluginId == pluginId; }), g_payloads.end());
	for (auto& payload : payloads) {
		payload.pluginId = pluginId;
		g_payloads.push_back(std::move(payload));
	}
}

void ClearPluginPayloads(const std::string& pluginId) {
	std::lock_guard<std::mutex> lock(g_payload_mutex);
	g_payloads.erase(std::remove_if(g_payloads.begin(), g_payloads.end(),
		[&](const InjectionPayload& payload) { return payload.pluginId == pluginId; }), g_payloads.end());
}

std::vector<InjectionPayload> PluginPayloads() {
	std::lock_guard<std::mutex> lock(g_payload_mutex);
	return g_payloads;
}

bool FindPluginPayload(const std::string& id, InjectionPayload& out) {
	std::lock_guard<std::mutex> lock(g_payload_mutex);
	for (const auto& payload : g_payloads) {
		if (payload.id == id) {
			out = payload;
			return true;
		}
	}
	return false;
}

bool ApplyPayloadBytes(const InjectionPayload& payload,
	bool (*write)(void* context, uint32_t address, uint8_t byte), void* context, std::string& error) {
	if (!write) {
		error = "no memory writer is available";
		return false;
	}
	for (size_t i = 0; i < payload.bytes.size(); ++i) {
		if (!write(context, payload.address + static_cast<uint32_t>(i), payload.bytes[i])) {
			std::ostringstream stream;
			stream << std::hex << (payload.address + static_cast<uint32_t>(i));
			error = "cannot write payload byte at 0x" + stream.str();
			return false;
		}
	}
	return true;
}

} // namespace casioemu::plugin
