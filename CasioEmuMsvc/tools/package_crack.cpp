// Standalone .package password recovery tool.
// Build (macOS/Linux):
//   clang++ -std=c++20 -O3 -pthread \
//     -I CasioEmuMsvc/src -I CasioEmuMsvc/src/Ext \
//     CasioEmuMsvc/tools/package_crack.cpp -o CasioEmuMsvc/tools/package_crack
#include "Ext/PackageCracker.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static void print_usage(const char* prog) {
	std::cout
		<< "Usage: " << prog << " <file.package> [options]\n"
		<< "\n"
		<< "Recovers the password of an encrypted .package file by offline guessing.\n"
		<< "Note: the password is not stored and cannot be mathematically reversed.\n"
		<< "\n"
		<< "Options:\n"
		<< "  -w, --wordlist <path>  Dictionary attack using a newline-separated word list\n"
		<< "  -c, --charset <chars>  Enable mask attack. e.g. -c 0123456789 or -c abc...\n"
		<< "      --min <n>          Mask minimum length              (default 1)\n"
		<< "      --max <n>          Mask maximum length              (default 6)\n"
		<< "      --prefix <str>     Fixed prefix for mask candidates\n"
		<< "      --suffix <str>     Fixed suffix for mask candidates\n"
		<< "  -p, --pattern <str>    Pattern attack; wildcard = '?' by default\n"
		<< "      --wildcard <char>  Character used as wildcard in --pattern (default '?')\n"
		<< "      --pcharset <chars> Charset for --pattern wildcards   (default a-z)\n"
		<< "  -t, --threads <n>      Worker threads (default: all cores)\n"
		<< "      --no-defaults      Do not try the built-in default passwords\n"
		<< "  -h, --help             Show this help\n";
}

static std::vector<std::string> read_wordlist(const std::string& path) {
	std::vector<std::string> words;
	std::ifstream f(path);
	if (!f) {
		std::cerr << "[!] cannot open word list: " << path << "\n";
		return words;
	}
	std::string line;
	while (std::getline(f, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		words.push_back(line);
	}
	return words;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		print_usage(argv[0]);
		return 1;
	}
	for (int i = 1; i < argc; ++i) {
		if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
			print_usage(argv[0]);
			return 0;
		}
	}

	std::string file = argv[1];
	std::string wordlist_path;
	std::string charset, prefix, suffix;
	std::string pattern, pcharset = "abcdefghijklmnopqrstuvwxyz";
	char wildcard = '?';
	int min_len = 1, max_len = 6;
	unsigned threads = 0;
	bool use_defaults = true;
	bool have_mask = false, have_wordlist = false, have_pattern = false;

	for (int i = 2; i < argc; ++i) {
		std::string a = argv[i];
		auto need = [&](const char* name) -> std::string {
			if (i + 1 >= argc) {
				std::cerr << "[!] " << name << " requires a value\n";
				std::exit(1);
			}
			return argv[++i];
		};
		if (a == "-w" || a == "--wordlist") { wordlist_path = need("--wordlist"); have_wordlist = true; }
		else if (a == "-c" || a == "--charset") { charset = need("--charset"); have_mask = true; }
		else if (a == "--min") { min_len = std::stoi(need("--min")); }
		else if (a == "--max") { max_len = std::stoi(need("--max")); }
		else if (a == "--prefix") { prefix = need("--prefix"); }
		else if (a == "--suffix") { suffix = need("--suffix"); }
		else if (a == "-p" || a == "--pattern") { pattern = need("--pattern"); have_pattern = true; }
		else if (a == "--wildcard") { std::string w = need("--wildcard"); wildcard = w.empty() ? '?' : w[0]; }
		else if (a == "--pcharset") { pcharset = need("--pcharset"); }
		else if (a == "-t" || a == "--threads") { threads = (unsigned)std::stoul(need("--threads")); }
		else if (a == "--no-defaults") { use_defaults = false; }
		else {
			std::cerr << "[!] unknown argument: " << a << "\n";
			return 1;
		}
	}

	// --- Load the package -------------------------------------------------
	pk_crack::Target tgt;
	try {
		ReadFile(file, tgt.pkg);
	} catch (const std::exception& e) {
		std::cerr << "[!] failed to read package: " << e.what() << "\n";
		return 1;
	}

	std::cout << "[*] file          : " << file << "\n";
	std::cout << "[*] encVersion    : " << (int)tgt.pkg.EncryptionVersion
	          << (tgt.is_v2() ? " (PBKDF2-HMAC-SHA256 + ChaCha20 + HMAC)" : " (legacy XOR + CRC32)") << "\n";
	std::cout << "[*] encrypted     : " << (tgt.pkg.IsEncrypted ? "yes" : "no") << "\n";
	std::cout << "[*] rom/flash/ifc : " << tgt.pkg.RomData.size() << " / "
	          << tgt.pkg.FlashData.size() << " / " << tgt.pkg.InterfaceData.size() << " bytes\n";

	if (!tgt.pkg.IsEncrypted) {
		std::cout << "[*] package is not encrypted - no password needed.\n";
		return 0;
	}
	tgt.prepare();

	auto t_start = std::chrono::steady_clock::now();
	auto report = [&](const std::string& pw) {
		auto t_end = std::chrono::steady_clock::now();
		double secs = std::chrono::duration<double>(t_end - t_start).count();
		std::cout << "\n[+] PASSWORD FOUND: \"" << pw << "\"  (" << secs << " s)\n";
	};

	// --- Built-in defaults -----------------------------------------------
	if (use_defaults) {
		static const char* defaults[] = {
			"0x0d000721", "0x0D000721", "0d000721", "0D000721", "0x0D000721",
			"password", "123456", "admin", "test", "abc", "casio", "1234", "0000"
		};
		std::vector<std::string> d(defaults, defaults + sizeof(defaults) / sizeof(defaults[0]));
		std::cout << "[*] trying " << d.size() << " built-in defaults...\n";
		std::string pw;
		if (pk_crack::crack_wordlist(tgt, d, threads, pw)) {
			report(pw);
			return 0;
		}
	}

	// --- Dictionary ------------------------------------------------------
	if (have_wordlist) {
		auto words = read_wordlist(wordlist_path);
		std::cout << "[*] dictionary: " << words.size() << " entries from " << wordlist_path << "\n";
		std::string pw;
		if (pk_crack::crack_wordlist(tgt, words, threads, pw)) {
			report(pw);
			return 0;
		}
		std::cout << "[-] dictionary exhausted\n";
	}

	// --- Pattern ---------------------------------------------------------
	if (have_pattern) {
		std::cout << "[*] pattern: \"" << pattern << "\" wildcard='" << wildcard
		          << "' charset=" << pcharset << "\n";
		std::string pw;
		if (pk_crack::crack_pattern(tgt, pattern, wildcard, pcharset, threads, pw)) {
			report(pw);
			return 0;
		}
		std::cout << "[-] pattern space exhausted\n";
	}

	// --- Mask ------------------------------------------------------------
	if (have_mask) {
		std::cout << "[*] mask: charset=\"" << charset << "\" len=" << min_len << ".." << max_len
		          << " prefix=\"" << prefix << "\" suffix=\"" << suffix << "\"\n";
		std::string pw;
		if (pk_crack::crack_mask(tgt, charset, min_len, max_len, prefix, suffix, threads, pw)) {
			report(pw);
			return 0;
		}
		std::cout << "[-] mask space exhausted\n";
	}

	std::cout << "\n[-] password not found.\n";
	return 2;
}
