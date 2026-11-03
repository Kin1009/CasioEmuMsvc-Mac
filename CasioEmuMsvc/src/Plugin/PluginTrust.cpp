#include "PluginTrust.h"

#include "../../../McpPlugin/json.hpp"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <system_error>

namespace casioemu::plugin {
namespace {

using json = nlohmann::json;

constexpr const char* kTrustFileName = "plugin-trust.json";

std::vector<std::string> ReadStringArray(const json& object, const char* key) {
	std::vector<std::string> values;
	auto it = object.find(key);
	if (it == object.end() || !it->is_array())
		return values;
	for (const auto& item : *it) {
		if (item.is_string())
			values.push_back(item.get<std::string>());
	}
	return NormalizePermissions(values);
}

TrustHashEntry ReadHistoryEntry(const json& item) {
	TrustHashEntry entry;
	entry.sha256 = item.value("sha256", "");
	entry.version = item.value("version", "");
	entry.trustedAt = item.value("trusted_at", "");
	entry.permissions = ReadStringArray(item, "permissions");
	return entry;
}

json WriteHistoryEntry(const TrustHashEntry& entry) {
	json item;
	item["sha256"] = entry.sha256;
	item["version"] = entry.version;
	item["trusted_at"] = entry.trustedAt;
	item["permissions"] = entry.permissions;
	return item;
}

} // namespace

bool SamePermissionSet(const std::vector<std::string>& lhs, const std::vector<std::string>& rhs) {
	return NormalizePermissions(lhs) == NormalizePermissions(rhs);
}

std::vector<std::string> NormalizePermissions(const std::vector<std::string>& permissions) {
	std::vector<std::string> normalized = permissions;
	std::sort(normalized.begin(), normalized.end());
	normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
	return normalized;
}

std::string CurrentUtcTimestamp() {
	const std::time_t now = std::time(nullptr);
	std::tm utc{};
#if defined(_WIN32)
	gmtime_s(&utc, &now);
#else
	gmtime_r(&now, &utc);
#endif
	char buffer[32]{};
	std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
	return buffer;
}

TrustEvaluation EvaluateTrust(const TrustRecord* record, bool native, const std::string& sha256,
	const std::vector<std::string>& permissions) {
	TrustEvaluation evaluation;
	if (!record) {
		evaluation.decision = TrustDecision::NoRecord;
		return evaluation;
	}
	if (record->blocked) {
		evaluation.decision = TrustDecision::Blocked;
		return evaluation;
	}
	if (!record->enabled) {
		evaluation.decision = TrustDecision::Disabled;
		return evaluation;
	}

	evaluation.hashChanged = record->sha256 != sha256;
	evaluation.permissionsChanged = !SamePermissionSet(record->permissions, permissions);
	evaluation.hashKnown = record->sha256 == sha256;
	for (const auto& entry : record->history) {
		if (entry.sha256 == sha256)
			evaluation.hashKnown = true;
	}

	if (evaluation.permissionsChanged) {
		evaluation.decision = TrustDecision::Prompt;
		return evaluation;
	}
	// Native code is never silently replaced.  Declarative documents with the
	// same permissions keep the old "no new prompt" behaviour.
	if (native && !evaluation.hashKnown) {
		evaluation.decision = TrustDecision::Prompt;
		return evaluation;
	}
	evaluation.decision = TrustDecision::Trusted;
	return evaluation;
}

bool TrustStore::Load(const std::filesystem::path& path, std::string* error) {
	m_records.clear();
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		if (error)
			error->clear();
		return false;
	}
	try {
		json document;
		file >> document;
		if (!document.is_object() || document.value("schema", 0) != 1) {
			if (error)
				*error = "unsupported trust store schema";
			return false;
		}
		if (!document.contains("records") || !document["records"].is_array()) {
			if (error)
				*error = "trust store is missing the 'records' array";
			return false;
		}
		for (const auto& item : document["records"]) {
			if (!item.is_object())
				continue;
			TrustRecord record;
			record.id = item.value("id", "");
			record.version = item.value("version", "");
			record.sha256 = item.value("sha256", "");
			record.trustedAt = item.value("trusted_at", "");
			record.enabled = item.value("enabled", true);
			record.blocked = item.value("blocked", false);
			record.permissions = ReadStringArray(item, "permissions");
			if (item.contains("history") && item["history"].is_array()) {
				for (const auto& history : item["history"]) {
					if (history.is_object())
						record.history.push_back(ReadHistoryEntry(history));
				}
			}
			// Migrate a v1 record (no history) into the per-hash trail.
			if (record.history.empty() && !record.sha256.empty()) {
				record.history.push_back({ record.sha256, record.version, record.trustedAt, record.permissions });
			}
			if (!record.id.empty())
				m_records.push_back(std::move(record));
		}
		if (error)
			error->clear();
		return true;
	}
	catch (const json::exception& exception) {
		if (error)
			*error = exception.what();
		return false;
	}
}

bool TrustStore::Save(const std::filesystem::path& path, std::string* error) const {
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	json document;
	document["schema"] = 1;
	document["records"] = json::array();
	for (const auto& record : m_records) {
		json item;
		item["id"] = record.id;
		item["version"] = record.version;
		item["sha256"] = record.sha256;
		item["trusted_at"] = record.trustedAt;
		item["permissions"] = NormalizePermissions(record.permissions);
		item["enabled"] = record.enabled;
		item["blocked"] = record.blocked;
		json history = json::array();
		for (const auto& entry : record.history)
			history.push_back(WriteHistoryEntry(entry));
		item["history"] = std::move(history);
		document["records"].push_back(std::move(item));
	}

	const std::filesystem::path temp = path.string() + ".tmp";
	{
		std::ofstream file(temp, std::ios::binary | std::ios::trunc);
		if (!file) {
			if (error)
				*error = "cannot write " + temp.string();
			return false;
		}
		file << document.dump(2);
		file.close();
	}
	std::filesystem::rename(temp, path, ec);
	if (ec) {
		std::filesystem::remove(path, ec);
		ec.clear();
		std::filesystem::rename(temp, path, ec);
	}
	if (ec) {
		if (error)
			*error = ec.message();
		return false;
	}
	return true;
}

TrustRecord* TrustStore::Find(const std::string& id) {
	for (auto& record : m_records) {
		if (record.id == id)
			return &record;
	}
	return nullptr;
}

const TrustRecord* TrustStore::Find(const std::string& id) const {
	for (const auto& record : m_records) {
		if (record.id == id)
			return &record;
	}
	return nullptr;
}

void TrustStore::Upsert(TrustRecord record) {
	record.permissions = NormalizePermissions(record.permissions);
	record.trustedAt = CurrentUtcTimestamp();

	if (record.blocked) {
		record.history.clear();
	}
	else if (!record.sha256.empty()) {
		auto it = std::find_if(record.history.begin(), record.history.end(),
			[&](const TrustHashEntry& entry) { return entry.sha256 == record.sha256; });
		TrustHashEntry entry{ record.sha256, record.version, record.trustedAt, record.permissions };
		if (it == record.history.end())
			record.history.push_back(std::move(entry));
		else
			*it = std::move(entry);
		while (record.history.size() > kMaxHistoryEntries)
			record.history.erase(record.history.begin());
	}

	TrustRecord* existing = Find(record.id);
	if (!existing) {
		m_records.push_back(std::move(record));
		return;
	}
	*existing = std::move(record);
}

void TrustStore::Remove(const std::string& id) {
	m_records.erase(std::remove_if(m_records.begin(), m_records.end(),
		[&](const TrustRecord& record) { return record.id == id; }), m_records.end());
}

} // namespace casioemu::plugin
