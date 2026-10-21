#!/usr/bin/env python3
"""Word-segmentation attack for a masked password.

Given a pattern where '?' is an unknown lowercase letter, find every way to
tile the string with dictionary words (concatenated, no separators).
"""
import argparse
import functools
import sys


def load_words(paths, lo, hi):
    ws = set()
    for p in paths:
        try:
            with open(p, encoding="latin1") as fh:
                for line in fh:
                    w = line.strip().lower()
                    if w.isalpha() and w.isascii() and lo <= len(w) <= hi:
                        ws.add(w)
        except FileNotFoundError:
            sys.stderr.write(f"[!] missing wordlist {p}\n")
    return sorted(ws)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pattern")
    ap.add_argument("wordlists", nargs="+")
    ap.add_argument("--min", type=int, default=3)
    ap.add_argument("--max-words", type=int, default=0, help="only tilings with <= N words")
    ap.add_argument("--cap", type=int, default=50_000_000)
    ap.add_argument("-o", "--out")
    args = ap.parse_args()

    pat = args.pattern.replace("_", "?")
    N = len(pat)
    words = load_words(args.wordlists, args.min, N)
    maxw = args.max_words if args.max_words > 0 else N
    sys.stderr.write(f"[*] pattern {pat!r} len={N} words={len(words)} max_words={maxw}\n")

    by_first = {}
    for w in words:
        by_first.setdefault(w[0], []).append(w)

    def matched(w, pos):
        if len(w) > N - pos:
            return False
        for j in range(len(w)):
            c = pat[pos + j]
            if c != "?" and c != w[j]:
                return False
        return True

    @functools.lru_cache(maxsize=None)
    def count(pos, k):
        if pos == N:
            return 1
        if k == 0:
            return 0
        tot = 0
        first = pat[pos]
        cand = by_first.get(first, []) if first != "?" else words
        for w in cand:
            if matched(w, pos):
                tot += count(pos + len(w), k - 1)
                if tot > args.cap:
                    return tot
        return tot

    total = count(0, maxw)
    sys.stderr.write(f"[*] tilings = {total:,}\n")
    if total == 0:
        return
    if total > args.cap:
        sys.stderr.write(f"[!] too many to enumerate (cap {args.cap:,}); need a smaller/tighter list\n")
        return
    if not args.out:
        sys.stderr.write("[!] within cap but no -o given; nothing written\n")
        return

    out = open(args.out, "w")
    n = 0

    def emit(pos, acc, k):
        nonlocal n
        if pos == N:
            out.write("".join(acc) + "\n")
            n += 1
            return
        if k == 0:
            return
        first = pat[pos]
        cand = by_first.get(first, []) if first != "?" else words
        for w in cand:
            if matched(w, pos) and count(pos + len(w), k - 1) > 0:
                acc.append(w)
                emit(pos + len(w), acc, k - 1)
                acc.pop()

    emit(0, [], maxw)
    out.close()
    sys.stderr.write(f"[*] wrote {n:,} candidates to {args.out}\n")


if __name__ == "__main__":
    main()
