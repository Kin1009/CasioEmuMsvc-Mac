#pragma once
/*
	Package password recovery (brute force / dictionary) for CasioEmuMsvc.

	This does NOT "reverse" the encryption - the password is never stored.
	It exploits the fact that Decrypt() verifies the password with a cheap,
	offline oracle (HMAC for v2, CRC32 for the legacy v1 XOR scheme), so we can
	test unlimited guesses locally with no false positives.

	Reuses the crypto:: primitives from RomPackage.h.
*/
#include "RomPackage.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Optional accelerated backend. Produces byte-identical PBKDF2/HMAC results,
// just much faster than the hand-rolled SHA256. Define PK_USE_OPENSSL to enable.
#if defined(PK_USE_OPENSSL)
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#endif

namespace pk_crack {

#if defined(PK_USE_OPENSSL)
// ---------------------------------------------------------------------------
// Fast single-block PBKDF2-HMAC-SHA256 (32-byte output). Drives the low-level
// SHA-256 directly with precomputed ipad/opad midstates, avoiding OpenSSL 3's
// per-call EVP overhead. ~3x faster than PKCS5_PBKDF2_HMAC on this machine and
// byte-identical.
// ---------------------------------------------------------------------------
inline void fast_pbkdf2_sha256(const unsigned char* pw, size_t pwlen,
	const unsigned char* salt, size_t saltlen, uint32_t iters, unsigned char out[32]) {
	unsigned char k0[64];
	std::memset(k0, 0, sizeof(k0));
	if (pwlen <= 64) {
		std::memcpy(k0, pw, pwlen);
	} else {
		unsigned char d[32];
		SHA256(pw, pwlen, d);
		std::memcpy(k0, d, 32);
	}
	unsigned char ipad[64], opad[64];
	for (int i = 0; i < 64; ++i) { ipad[i] = k0[i] ^ 0x36; opad[i] = k0[i] ^ 0x5c; }
	SHA256_CTX ib, ob, ic, oc;
	SHA256_Init(&ib); SHA256_Update(&ib, ipad, 64);
	SHA256_Init(&ob); SHA256_Update(&ob, opad, 64);
	unsigned char sb[80];
	std::memcpy(sb, salt, saltlen);
	sb[saltlen] = 0; sb[saltlen + 1] = 0; sb[saltlen + 2] = 0; sb[saltlen + 3] = 1;
	unsigned char inner[32], U[32], acc[32];
	ic = ib; SHA256_Update(&ic, sb, saltlen + 4); SHA256_Final(inner, &ic);
	oc = ob; SHA256_Update(&oc, inner, 32); SHA256_Final(U, &oc);
	std::memcpy(acc, U, 32);
	for (uint32_t it = 1; it < iters; ++it) {
		ic = ib; SHA256_Update(&ic, U, 32); SHA256_Final(inner, &ic);
		oc = ob; SHA256_Update(&oc, inner, 32); SHA256_Final(U, &oc);
		for (int k = 0; k < 32; ++k) acc[k] ^= U[k];
	}
	std::memcpy(out, acc, 32);
}

inline void fast_hmac_sha256(const unsigned char* key, size_t keylen,
	const unsigned char* data, size_t datalen, unsigned char out[32]) {
	unsigned char k0[64];
	std::memset(k0, 0, sizeof(k0));
	if (keylen <= 64) {
		std::memcpy(k0, key, keylen);
	} else {
		unsigned char d[32];
		SHA256(key, keylen, d);
		std::memcpy(k0, d, 32);
	}
	unsigned char ipad[64], opad[64];
	for (int i = 0; i < 64; ++i) { ipad[i] = k0[i] ^ 0x36; opad[i] = k0[i] ^ 0x5c; }
	unsigned char inner[32];
	SHA256_CTX c;
	SHA256_Init(&c); SHA256_Update(&c, ipad, 64); SHA256_Update(&c, data, datalen); SHA256_Final(inner, &c);
	SHA256_Init(&c); SHA256_Update(&c, opad, 64); SHA256_Update(&c, inner, 32); SHA256_Final(out, &c);
}
#endif

// ---------------------------------------------------------------------------
// Raw CRC-32 (zlib polynomial), identical to RomPackage::crc32.
// ---------------------------------------------------------------------------
inline uint32_t crc32_calc(const unsigned char* data, size_t len) {
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; ++i) {
		crc ^= data[i];
		for (int b = 0; b < 8; ++b)
			crc = (crc >> 1) ^ (0xEDB88320u * (crc & 1u));
	}
	return ~crc;
}
inline uint32_t crc32_calc(const std::vector<unsigned char>& v) {
	return crc32_calc(v.data(), v.size());
}

// ---------------------------------------------------------------------------
// A package ready for testing. `auth` is the cached ciphertext concat for v2.
// ---------------------------------------------------------------------------
struct Target {
	RomPackage pkg;
	std::vector<uint8_t> auth;   // RomData || FlashData || InterfaceData (v2 only)

	bool is_v2() const { return pkg.EncryptionVersion == 2; }

	// Build from an already-parsed package.
	void prepare() {
		if (is_v2()) {
			auth.clear();
			auth.reserve(pkg.RomData.size() + pkg.FlashData.size() + pkg.InterfaceData.size());
			auth.insert(auth.end(), pkg.RomData.begin(), pkg.RomData.end());
			auth.insert(auth.end(), pkg.FlashData.begin(), pkg.FlashData.end());
			auth.insert(auth.end(), pkg.InterfaceData.begin(), pkg.InterfaceData.end());
		}
	}

	// v2 oracle: PBKDF2 -> K_mac -> HMAC compare.
	bool check_v2(const std::string& key) const {
#if defined(PK_USE_OPENSSL)
		unsigned char K[32];
		fast_pbkdf2_sha256((const unsigned char*)key.data(), key.size(),
			pkg.Salt.data(), pkg.Salt.size(), 10000, K);
		unsigned char kin[35];
		std::memcpy(kin, K, 32);
		kin[32] = 'M'; kin[33] = 'A'; kin[34] = 'C';
		unsigned char K_mac[32];
		SHA256(kin, sizeof(kin), K_mac);
		unsigned char h[32];
		fast_hmac_sha256(K_mac, 32, auth.data(), auth.size(), h);
		return pkg.Hmac.size() == 32 && CRYPTO_memcmp(h, pkg.Hmac.data(), 32) == 0;
#else
		std::vector<uint8_t> K = crypto::pbkdf2_hmac_sha256(key, pkg.Salt, 10000);
		static const std::vector<uint8_t> mac_label = { 'M', 'A', 'C' };
		std::vector<uint8_t> kin = K;
		kin.insert(kin.end(), mac_label.begin(), mac_label.end());
		std::vector<uint8_t> K_mac = crypto::SHA256::hash256(kin);
		std::vector<uint8_t> h = crypto::HMAC_SHA256::hmac(K_mac, auth);
		return crypto::constant_time_compare(h, pkg.Hmac);
#endif
	}

	// v1 oracle: masked-XOR stream validated with the stored Crc32.
	// Computes the decrypted CRC on the fly (no large copies).
	bool check_v1(const std::string& key) const {
		if (key.empty())
			return false;
		uint32_t total = part_crc(pkg.RomData, key) ^ part_crc(pkg.FlashData, key) ^ part_crc(pkg.InterfaceData, key);
		return total == pkg.Crc32;
	}

	bool check(const std::string& key) const {
		if (is_v2())
			return check_v2(key);
		return check_v1(key);
	}

private:
	static uint32_t part_crc(const std::vector<unsigned char>& data, const std::string& key) {
		const std::vector<unsigned char> kb(key.begin(), key.end());
		uint32_t seed = crc32_calc(kb);
		uint32_t crc = 0xFFFFFFFFu;
		const size_t klen = key.length();
		for (size_t i = 0; i < data.size(); ++i) {
			seed = (seed * 86028121u + 611953u) & 0xFFFFFFFFu;
			unsigned char mask = (seed ^ (seed >> 8) ^ (seed >> 16) ^ (seed >> 24)) & 0xFF;
			unsigned char byte = data[i] ^ (unsigned char)(key[i % klen] ^ mask);
			crc ^= byte;
			for (int b = 0; b < 8; ++b)
				crc = (crc >> 1) ^ (0xEDB88320u * (crc & 1u));
		}
		return ~crc;
	}
};

// ---------------------------------------------------------------------------
// Parallel driver. `next_index` produces candidate i, `total` is the count.
// Returns true and fills `out` on success.
// ---------------------------------------------------------------------------
template <class Gen>
bool run_parallel(size_t total, unsigned threads, const Target& tgt, Gen&& gen, std::string& out,
	const char* label = nullptr) {
	if (threads == 0)
		threads = std::thread::hardware_concurrency();
	if (threads == 0)
		threads = 4;

	std::atomic<size_t> next{ 0 };
	std::atomic<bool> stop{ false };
	std::atomic<bool> done{ false };
	std::mutex mtx;
	std::string found;

	auto t_start = std::chrono::steady_clock::now();

	std::thread reporter;
	if (label && total > 0) {
		reporter = std::thread([&]() {
			size_t last = 0;
			while (!done.load(std::memory_order_relaxed)) {
				std::this_thread::sleep_for(std::chrono::seconds(2));
				size_t p = next.load(std::memory_order_relaxed);
				double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
				double rate = secs > 0 ? p / secs : 0;
				double eta = rate > 0 ? (double)(total - p) / rate : 0;
				std::fprintf(stderr, "\r[%s] %zu/%zu (%.3f%%) %.0f/s  ETA %.0fs   ",
					label, p < total ? p : total, total, 100.0 * p / (double)total, rate, eta);
				std::fflush(stderr);
				last = p;
			}
		});
	}

	auto worker = [&]() {
		for (;;) {
			size_t i = next.fetch_add(1, std::memory_order_relaxed);
			if (i >= total || stop.load(std::memory_order_relaxed))
				return;
			std::string cand = gen(i);
			if (tgt.check(cand)) {
				{
					std::lock_guard<std::mutex> lk(mtx);
					found = cand;
				}
				stop.store(true, std::memory_order_relaxed);
				return;
			}
		}
	};

	std::vector<std::thread> pool;
	for (unsigned i = 1; i < threads; ++i)
		pool.emplace_back(worker);
	worker();
	for (auto& th : pool)
		th.join();

	done.store(true);
	if (reporter.joinable())
		reporter.join();
	if (label)
		std::fprintf(stderr, "\n");

	if (stop.load()) {
		out = found;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Dictionary attack from an in-memory word list.
// ---------------------------------------------------------------------------
inline bool crack_wordlist(const Target& tgt, const std::vector<std::string>& words,
	unsigned threads, std::string& out) {
	return run_parallel(words.size(), threads, tgt,
		[&](size_t i) { return words[i]; }, out, "dict");
}

// ---------------------------------------------------------------------------
// Pattern attack: literal characters are fixed, `wild` marks a free position
// drawn from `charset`. e.g. pattern "c?c??c" with wild='?' and charset "a..z".
// ---------------------------------------------------------------------------
inline bool crack_pattern(const Target& tgt, const std::string& pattern, char wild,
	const std::string& charset, unsigned threads, std::string& out) {
	std::vector<size_t> pos;
	for (size_t i = 0; i < pattern.size(); ++i)
		if (pattern[i] == wild)
			pos.push_back(i);

	if (pos.empty()) {
		if (tgt.check(pattern)) { out = pattern; return true; }
		return false;
	}
	if (charset.empty())
		return false;

	unsigned long long total = 1;
	for (size_t k = 0; k < pos.size(); ++k) {
		if (total > (1ULL << 62) / charset.size()) {
			std::fprintf(stderr, "[!] pattern space too large to enumerate (2^%.1f)\n",
				pos.size() * std::log2((double)charset.size()));
			return false;
		}
		total *= charset.size();
	}
	std::fprintf(stderr, "[*] pattern: %zu wildcards, charset %zu -> %llu candidates\n",
		pos.size(), charset.size(), total);

	return run_parallel((size_t)total, threads, tgt, [&](size_t idx) {
		std::string s = pattern;
		unsigned long long v = idx;
		for (size_t k = pos.size(); k-- > 0;) {
			s[pos[k]] = charset[v % charset.size()];
			v /= charset.size();
		}
		return s;
	}, out, "pattern");
}

// ---------------------------------------------------------------------------
// Mask/brute-force attack over `charset` for lengths [min_len, max_len].
// ---------------------------------------------------------------------------
inline bool crack_mask(const Target& tgt, const std::string& charset, int min_len, int max_len,
	const std::string& prefix, const std::string& suffix, unsigned threads, std::string& out) {
	if (charset.empty() || min_len < 0 || max_len < min_len)
		return false;

	for (int len = min_len; len <= max_len; ++len) {
		// total = charset^len (bail out on overflow)
		unsigned long long total = 1;
		bool overflow = false;
		for (int i = 0; i < len; ++i) {
			if (total > (1ULL << 62) / charset.size()) { overflow = true; break; }
			total *= charset.size();
		}
		if (overflow) {
			std::fprintf(stderr, "[!] mask space for length %d is too large, skipping\n", len);
			continue;
		}
		std::fprintf(stderr, "[*] length %d: %llu candidates\n", len, total);
		bool found = run_parallel((size_t)total, threads, tgt,
			[&](size_t idx) {
				std::string s((size_t)len, charset[0]);
				unsigned long long v = idx;
				for (int i = len - 1; i >= 0; --i) {
					s[(size_t)i] = charset[v % charset.size()];
					v /= charset.size();
				}
				return prefix + s + suffix;
			}, out, "mask");
		if (found)
			return true;
	}
	return false;
}

} // namespace pk_crack
