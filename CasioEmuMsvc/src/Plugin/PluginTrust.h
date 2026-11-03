#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace casioemu::plugin {

// One approved package hash.  The trust store keeps a bounded history per
// plugin id so upgrades do not erase the audit trail and so the manager can
// show when each hash was approved.
struct TrustHashEntry {
	std::string sha256;
	std::string version;
	std::string trustedAt; // ISO-8601 UTC, e.g. 2026-11-02T12:34:56Z
	std::vector<std::string> permissions;
};

struct TrustRecord {
	std::string id;
	std::string version;
	std::string sha256;
	std::string trustedAt;
	std::vector<std::string> permissions;
	bool enabled = true;
	bool blocked = false;
	std::vector<TrustHashEntry> history;
};

enum class TrustDecision {
	NoRecord,  // first install: ask the user
	Trusted,   // load without prompting
	Prompt,    // hash or permission change: ask again
	Blocked,   // user blocked this id
	Disabled,  // user disabled this id
};

struct TrustEvaluation {
	TrustDecision decision = TrustDecision::NoRecord;
	bool hashChanged = false;
	bool permissionsChanged = false;
	bool hashKnown = false;
};

// Compares permission lists as sets (order-insensitive, duplicates removed).
[[nodiscard]] bool SamePermissionSet(const std::vector<std::string>& lhs,
	const std::vector<std::string>& rhs);
[[nodiscard]] std::vector<std::string> NormalizePermissions(const std::vector<std::string>& permissions);

[[nodiscard]] std::string CurrentUtcTimestamp();

// Pure policy function: given an existing record (or nullptr), the plugin kind
// and the freshly computed package hash / declared permissions, decide what
// the registry should do.  It never mutates the record.
[[nodiscard]] TrustEvaluation EvaluateTrust(const TrustRecord* record, bool native,
	const std::string& sha256, const std::vector<std::string>& permissions);

class TrustStore {
public:
	// Reads schema 1 documents.  Unknown keys are ignored; malformed records
	// are skipped.  A missing file is not an error (empty store).
	bool Load(const std::filesystem::path& path, std::string* error = nullptr);
	// Writes atomically (temp file + rename).
	bool Save(const std::filesystem::path& path, std::string* error = nullptr) const;

	[[nodiscard]] TrustRecord* Find(const std::string& id);
	[[nodiscard]] const TrustRecord* Find(const std::string& id) const;

	// Inserts or updates the record for `record.id`.  Permissions are
	// normalized, `trusted_at` is refreshed and the hash is appended to the
	// per-hash history (bounded to kMaxHistoryEntries).
	void Upsert(TrustRecord record);
	void Remove(const std::string& id);

	[[nodiscard]] std::vector<TrustRecord>& Records() { return m_records; }
	[[nodiscard]] const std::vector<TrustRecord>& Records() const { return m_records; }

	static constexpr size_t kMaxHistoryEntries = 16;

private:
	std::vector<TrustRecord> m_records;
};

} // namespace casioemu::plugin
