#!/usr/bin/env python3
"""Build a deterministic single-character/two-character ranking prior."""
from __future__ import annotations

import argparse
import collections
import hashlib
import math
import re
import struct
from pathlib import Path

MAGIC = b"CSLXPR1\0"
VERSION = 1
HEADER = struct.Struct("<8sII")
RECORD = struct.Struct("<HHI")
PINYIN = re.compile(r"^[a-z]+$")
BMP_CHINESE = re.compile(r"^[\u4e00-\u9fff]+$")
MAX_SCORE = 0xFFFFFFFF


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def read_character_frequencies(path: Path):
    with path.open(encoding="utf-8-sig", errors="strict") as source:
        for raw in source:
            stripped = raw.strip()
            if not stripped or stripped.startswith(("#", ";")) or stripped == "...":
                continue
            fields = stripped.split("\t")
            if len(fields) < 3:
                continue
            first, second = fields[0].strip(), fields[1].strip().lower()
            if PINYIN.fullmatch(first) and len(fields[1].strip()) == 1:
                pinyin, word = first, fields[1].strip()
            else:
                word, pinyin = fields[0].strip(), second
            if len(word) != 1 or not BMP_CHINESE.fullmatch(word):
                continue
            if not PINYIN.fullmatch(pinyin):
                continue
            try:
                frequency = int(float(fields[2].strip()))
            except ValueError:
                continue
            if frequency > 0:
                yield pinyin, word, frequency


def read_base_entries(path: Path):
    with path.open(encoding="utf-8-sig", errors="strict") as source:
        for raw in source:
            stripped = raw.strip()
            if not stripped or stripped.startswith(("#", ";")):
                continue
            fields = stripped.split("\t")
            if len(fields) != 3:
                continue
            pinyin, word = fields[0].strip().lower(), fields[1].strip()
            if not PINYIN.fullmatch(pinyin) or not BMP_CHINESE.fullmatch(word):
                continue
            try:
                frequency = int(fields[2].strip())
            except ValueError:
                continue
            if frequency > 0:
                yield pinyin, word, frequency


def build_records(char_source: Path, base_dictionary: Path):
    context_frequency: collections.Counter[str] = collections.Counter()
    records: dict[tuple[str, str], int] = {}
    for pinyin, word, frequency in read_base_entries(base_dictionary):
        for character in word:
            context_frequency[character] += max(1, frequency)
        if len(word) == 2 and frequency > 0:
            key = (pinyin, word)
            records[key] = max(records.get(key, 0), min(frequency, MAX_SCORE))

    char_frequency: dict[tuple[str, str], int] = {}
    for pinyin, word, frequency in read_character_frequencies(char_source):
        key = (pinyin, word)
        char_frequency[key] = max(char_frequency.get(key, 0), frequency)

    for key, frequency in char_frequency.items():
        context = max(frequency, context_frequency.get(key[1], 0))
        # The standalone table is useful for pronunciation coverage, while actual
        # word usage is the stronger ordering signal. A 1:3 geometric blend keeps
        # rare readings bounded without letting table counts dominate common usage.
        score = round(math.pow(frequency, 0.25) * math.pow(context, 0.75))
        records[key] = max(1, min(score, MAX_SCORE))

    return sorted((pinyin, word, score) for (pinyin, word), score in records.items())


def write_model(records, output: Path):
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    with temporary.open("wb") as stream:
        stream.write(HEADER.pack(MAGIC, VERSION, len(records)))
        for pinyin, word, score in records:
            pinyin_bytes = pinyin.encode("ascii")
            word_bytes = word.encode("utf-8")
            if not pinyin_bytes or len(pinyin_bytes) > 128:
                raise ValueError(f"pinyin field out of range: {pinyin!r}")
            if not word_bytes or len(word_bytes) > 256:
                raise ValueError(f"word field out of range: {word!r}")
            stream.write(RECORD.pack(len(pinyin_bytes), len(word_bytes), score))
            stream.write(pinyin_bytes)
            stream.write(word_bytes)
    temporary.replace(output)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--char-source", required=True)
    parser.add_argument("--base", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--expect-char-sha256", default="")
    args = parser.parse_args()

    char_source = Path(args.char_source)
    base_dictionary = Path(args.base)
    if not char_source.is_file():
        parser.error(f"missing character source: {char_source}")
    if not base_dictionary.is_file():
        parser.error(f"missing base dictionary: {base_dictionary}")
    actual_hash = sha256(char_source)
    if args.expect_char_sha256 and actual_hash != args.expect_char_sha256.upper():
        parser.error(
            f"character source SHA-256 mismatch: {actual_hash} != "
            f"{args.expect_char_sha256.upper()}"
        )

    records = build_records(char_source, base_dictionary)
    if not records:
        parser.error("no records generated")
    output = Path(args.out)
    write_model(records, output)
    lookup = {(pinyin, word): score for pinyin, word, score in records}
    print(
        f"records={len(records)} bytes={output.stat().st_size} "
        f"xiang/想={lookup.get(('xiang', '想'), 0)} "
        f"xian/现={lookup.get(('xian', '现'), 0)} -> {output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
