// wordgen.cpp - enumerate (or count) dictionary-word segmentations of a masked
// password, ordered by a word-frequency score (lower = more common first) or by
// a word-bigram language model (most probable phrase first).
//
//   wordgen <pattern> <max_words> <out> <vocab...> [--rank <rfile>...]
//           [--min N] [--cap N] [--count]
//           [--unigram <file>] [--bigram <file>]
//
// Vocab files supply the tiling words (one per line, extra tokens like counts
// are ignored). Rank files supply word frequency order for scoring; words absent
// from all rank files get rank 1,000,000. If no rank files are given, frequency
// = line order of concatenated vocab files.
//
// --unigram <file>: "word<tab>count" lines for unigram probabilities.
// --bigram  <file>: "w1 w2<tab>count" lines for bigram probabilities.
// When --bigram is given, candidates are sorted by phrase probability
// (most probable first) instead of by rank sum.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

static std::string norm(const std::string& in) {
	std::string s;
	for (unsigned char c : in) {
		if (c >= 'A' && c <= 'Z') s.push_back((char)(c - 'A' + 'a'));
		else if (c >= 'a' && c <= 'z') s.push_back((char)c);
	}
	return s;
}

int main(int argc, char** argv) {
	if (argc < 5) {
		std::fprintf(stderr,
			"usage: %s <pattern> <max_words> <out> <vocab...> [--rank <rfile>...] "
			"[--min N] [--cap N] [--count] [--unigram <file>] [--bigram <file>]\n", argv[0]);
		return 2;
	}
	std::string pattern = argv[1];
	for (char& c : pattern) if (c == '_') c = '?';
	const int NN = (int)pattern.size();
	const int maxw = atoi(argv[2]);
	const std::string outpath = argv[3];

	std::vector<std::string> vocabFiles, rankFiles;
	std::string unigramFile, bigramFile;
	std::vector<char> reqB(NN + 1, 0);   // reqB[i] = word boundary required after char i (1-based i)
	int minlen = 3;
	long long cap = 2000000000LL;
	bool countOnly = false;
	bool inRank = false;
	for (int i = 4; i < argc; ++i) {
		if (!strcmp(argv[i], "--rank")) { inRank = true; continue; }
		if (!strcmp(argv[i], "--min")) { minlen = atoi(argv[++i]); continue; }
		if (!strcmp(argv[i], "--cap")) { cap = atoll(argv[++i]); continue; }
		if (!strcmp(argv[i], "--count")) { countOnly = true; continue; }
		if (!strcmp(argv[i], "--unigram")) { unigramFile = argv[++i]; continue; }
		if (!strcmp(argv[i], "--bigram")) { bigramFile = argv[++i]; continue; }
		if (!strcmp(argv[i], "--boundary")) {
			std::string s = argv[++i], cur;
			for (size_t k = 0; k <= s.size(); ++k) {
				if (k == s.size() || s[k] == ',') { if (!cur.empty()) reqB[atoi(cur.c_str())] = 1; cur.clear(); }
				else cur.push_back(s[k]);
			}
			continue;
		}
		(inRank ? rankFiles : vocabFiles).push_back(argv[i]);
	}

	// --- vocabulary ---
	std::unordered_map<std::string, int> wid;
	std::vector<std::string> words;
	auto addWord = [&](const std::string& w) {
		if ((int)w.size() < minlen || (int)w.size() > NN) return;
		if (wid.count(w)) return;
		wid[w] = (int)words.size();
		words.push_back(w);
	};
	for (auto& f : vocabFiles) {
		std::ifstream in(f);
		std::string line;
		while (std::getline(in, line)) {
			std::istringstream ss(line);
			std::string w;
			ss >> w;             // first whitespace-delimited token
			addWord(norm(w));
		}
	}

	// --- language model ---
	std::unordered_map<std::string, double> uni;      // word -> count
	std::unordered_map<std::string, double> bi;       // w1 \x01 w2 -> count
	std::unordered_map<std::string, double> biSum;    // w1 -> sum count
	double uniTotal = 0.0;
	double V = 1.0;
	bool useLM = !bigramFile.empty();
	if (!unigramFile.empty()) {
		std::ifstream in(unigramFile);
		std::string line;
		while (std::getline(in, line)) {
			std::istringstream ss(line);
			std::string w; double c = 0;
			if (!(ss >> w >> c)) continue;
			std::string nw = norm(w);
			if (nw.empty()) continue;
			uni[nw] += c;
			uniTotal += c;
			V += 1.0;
		}
	}
	if (useLM) {
		std::ifstream in(bigramFile);
		std::string line;
		while (std::getline(in, line)) {
			std::istringstream ss(line);
			std::string w1, w2, extra; double c = 0;
			if (!(ss >> w1 >> w2 >> c)) continue;
			std::string n1 = norm(w1), n2 = norm(w2);
			if (n1.empty() || n2.empty()) continue;
			bi[n1 + "\x01" + n2] += c;
			biSum[n1] += c;
		}
	}
	auto logu = [&](const std::string& w) -> double {
		double c = 0; auto it = uni.find(w); if (it != uni.end()) c = it->second;
		return std::log((c + 1.0) / (uniTotal + V));
	};
	auto costWord = [&](const std::string& prev, const std::string& w) -> double {
		if (!useLM) return 0.0;
		if (prev.empty()) return -logu(w);
		auto it = bi.find(prev + "\x01" + w);
		double denom = 0; auto ip = uni.find(prev); if (ip != uni.end()) denom = ip->second;
		auto ib = biSum.find(prev); if (ib != biSum.end() && ib->second > denom) denom = ib->second;
		if (it != bi.end() && denom > 0) return -std::log(it->second / denom);
		return -std::log(0.01) - logu(w);   // backoff
	};

	// --- rank model (fallback scoring) ---
	std::unordered_map<std::string, long long> rank;
	long long r = 0;
	for (auto& f : rankFiles) {
		std::ifstream in(f);
		std::string line;
		while (std::getline(in, line)) {
			std::string w = norm(line);
			if (!w.empty() && !rank.count(w)) rank[w] = r;
			++r;
		}
	}
	if (rankFiles.empty()) {
		for (size_t i = 0; i < words.size(); ++i) rank[words[i]] = (long long)i;
	}
	auto wscore = [&](const std::string& w) -> long long {
		auto it = rank.find(w);
		return it == rank.end() ? 1000000LL : it->second;
	};

	std::fprintf(stderr, "[*] pattern %s N=%d maxw=%d min=%d vocab=%zu words LM=%d\n",
		pattern.c_str(), NN, maxw, minlen, words.size(), (int)useLM);

	// --- per-position candidate words ---
	std::vector<std::vector<int>> cands(NN);
	for (int p = 0; p < NN; ++p) {
		char pc = pattern[p];
		for (int wi = 0; wi < (int)words.size(); ++wi) {
			const std::string& w = words[wi];
			if ((int)w.size() > NN - p) continue;
			if (pc != '?' && pc != w[0]) continue;
			bool ok = true;
			for (int j = 1; j < (int)w.size(); ++j) {
				if (reqB[p + j]) { ok = false; break; }   // would cross a required boundary
				if (pattern[p + j] != '?' && pattern[p + j] != w[j]) { ok = false; break; }
			}
			if (ok) cands[p].push_back(wi);
		}
	}

	const int KW = maxw + 1;

	// --- count (memoized) ---
	std::vector<long long> cnt((size_t)(NN + 1) * KW, -1);
	std::function<long long(int,int)> countFn = [&](int p, int k) -> long long {
		if (p == NN) return 1;
		if (k == 0) return 0;
		long long& m = cnt[(size_t)p * KW + k];
		if (m >= 0) return m;
		long long t = 0;
		for (int wi : cands[p]) {
			t += countFn(p + (int)words[wi].size(), k - 1);
			if (t > cap) { t = cap; break; }
		}
		m = t;
		return t;
	};

	long long total = countFn(0, maxw);
	std::fprintf(stderr, "[*] tilings = %lld%s\n", total, total >= cap ? " (capped)" : "");

	if (countOnly) return 0;
	if (total == 0) { std::ofstream(outpath).close(); return 0; }

	// --- enumerate ---
	struct Item { double cost; std::string s; };
	std::vector<Item> items;
	items.reserve((size_t)std::min<long long>(total, 8000000));
	std::string acc;
	std::function<void(int,int,double,const std::string&)> dfs =
		[&](int p, int k, double cost, const std::string& prev) {
		if ((long long)items.size() >= cap) return;
		if (p == NN) { items.push_back({cost, acc}); return; }
		for (int wi : cands[p]) {
			int len = (int)words[wi].size();
			if (countFn(p + len, k - 1) == 0) continue;
			size_t save = acc.size();
			acc += words[wi];
			double nc = useLM ? cost + costWord(prev, words[wi])
			                  : cost + (double)wscore(words[wi]);
			dfs(p + len, k - 1, nc, words[wi]);
			acc.resize(save);
		}
	};
	dfs(0, maxw, 0.0, std::string());
	std::fprintf(stderr, "[*] %zu candidates; scoring+sorting...\n", items.size());

	std::sort(items.begin(), items.end(),
		[](const Item& a, const Item& b) { return a.cost < b.cost; });

	std::ofstream out(outpath);
	std::string prev;
	for (auto& it : items) {
		if (it.s == prev) continue;
		prev = it.s;
		out << it.s << '\n';
	}
	std::fprintf(stderr, "[*] wrote %s\n", outpath.c_str());
	return 0;
}
