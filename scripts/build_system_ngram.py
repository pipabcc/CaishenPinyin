#!/usr/bin/env python3
"""Build the compact system character n-gram model from pinned Rime dictionaries."""
from __future__ import annotations

import argparse
import collections
import re
import struct
from pathlib import Path

MAGIC = b"CSNGRM1\0"
VERSION = 1
CHINESE_RUN = re.compile(r"[\u4e00-\u9fff]{2,16}")


def read_words(path: Path):
    in_data = False
    with path.open(encoding="utf-8", errors="strict") as source:
        for raw in source:
            text = raw.rstrip("\r\n")
            if not in_data:
                if text.strip() == "...":
                    in_data = True
                continue
            stripped = text.strip()
            if not stripped or stripped.startswith("#"):
                continue
            word = text.split("\t", 1)[0].strip()
            yield from CHINESE_RUN.findall(word)


def gram_key(gram: str) -> int:
    key = 0
    for char in gram:
        key = (key << 16) | ord(char)
    return key


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--src", action="append", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--min-bigram-count", type=int, default=2)
    parser.add_argument("--min-trigram-count", type=int, default=2)
    args = parser.parse_args()

    counts = {2: collections.Counter(), 3: collections.Counter()}
    seen_words: set[str] = set()
    for source_name in args.src:
        source = Path(source_name)
        if not source.is_file():
            parser.error(f"missing source: {source}")
        for word in read_words(source):
            if word in seen_words:
                continue
            seen_words.add(word)
            for size in (2, 3):
                for offset in range(len(word) - size + 1):
                    counts[size][word[offset : offset + size]] += 1

    bigrams = sorted(
        (gram_key(gram), count)
        for gram, count in counts[2].items()
        if count >= args.min_bigram_count
    )
    trigrams = sorted(
        (gram_key(gram), count)
        for gram, count in counts[3].items()
        if count >= args.min_trigram_count
    )

    output = Path(args.out)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    with temporary.open("wb") as stream:
        stream.write(struct.pack("<8sIII", MAGIC, VERSION, len(bigrams), len(trigrams)))
        for key, count in bigrams:
            stream.write(struct.pack("<II", key, min(count, 0xFFFFFFFF)))
        for key, count in trigrams:
            stream.write(struct.pack("<QI", key, min(count, 0xFFFFFFFF)))
    temporary.replace(output)
    print(
        f"words={len(seen_words)} bigrams={len(bigrams)} "
        f"trigrams={len(trigrams)} bytes={output.stat().st_size} -> {output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
