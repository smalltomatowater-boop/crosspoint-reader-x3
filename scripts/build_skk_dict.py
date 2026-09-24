#!/usr/bin/env python3
"""Convert an SKK dictionary (e.g. SKK-JISYO.L) into the editor's lookup file.

The editor binary-searches this file directly on the SD card, one line per
reading, so it never has to hold the dictionary in RAM. Output format:

    <reading> /<candidate>/<candidate>/.../\n

- UTF-8, LF line endings, no comment lines.
- Lines sorted by the UTF-8 bytes of <reading> (the firmware compares bytes).
- Annotations (";..." inside a candidate) and Lisp candidates ("(concat ...)")
  are removed; duplicate readings are merged.
- Only readings made of hiragana, with an optional trailing ASCII okurigana
  letter (SKK okuri-ari keys such as "かk"), are kept. Abbrev-mode entries
  (ASCII readings) and number/prefix/suffix entries (#, >, <) are dropped.

SKK-JISYO.L is GPL; this script ships with the firmware, the dictionary does
not. Get it from https://github.com/skk-dev/dict and copy the output to the
SD card at /dict/skk.txt.

Usage:
    python3 scripts/build_skk_dict.py SKK-JISYO.L skk.txt [--encoding euc-jp]
"""

import argparse
import sys


def is_hiragana(ch: str) -> bool:
    return "ぁ" <= ch <= "ゟ" or ch == "ー"


def keep_reading(reading: str) -> bool:
    if not reading:
        return False
    body = reading
    if reading[-1].isascii():
        if not reading[-1].isalpha() or len(reading) < 2:
            return False
        body = reading[:-1]
    return all(is_hiragana(ch) for ch in body)


def parse_candidates(field: str) -> list[str]:
    out = []
    for cand in field.strip("/").split("/"):
        cand = cand.split(";", 1)[0]
        if not cand or cand.startswith("("):
            continue
        out.append(cand)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--encoding", default="euc-jp")
    args = ap.parse_args()

    entries: dict[str, list[str]] = {}
    with open(args.input, encoding=args.encoding, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith(";"):
                continue
            reading, sep, rest = line.partition(" ")
            if not sep or not keep_reading(reading):
                continue
            cands = parse_candidates(rest)
            if not cands:
                continue
            merged = entries.setdefault(reading, [])
            for c in cands:
                if c not in merged:
                    merged.append(c)

    keys = sorted(entries, key=lambda k: k.encode("utf-8"))
    with open(args.output, "w", encoding="utf-8", newline="\n") as f:
        for k in keys:
            f.write(k + " /" + "/".join(entries[k]) + "/\n")

    longest = max(len((k + " /" + "/".join(entries[k]) + "/").encode("utf-8")) for k in keys)
    print(f"{len(keys)} readings written to {args.output} (longest line {longest} bytes)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
