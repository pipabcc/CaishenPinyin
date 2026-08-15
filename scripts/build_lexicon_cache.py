#!/usr/bin/env python3
"""Build the deterministic FCPYLEX1 cache consumed by lexicon_cache.cpp."""
from __future__ import annotations

import argparse
import hashlib
import os
import struct
from pathlib import Path

MAGIC = b"FCPYLEX1"
VERSION = 1
HEADER = struct.Struct("<8sII32s32s")
RECORD = struct.Struct("<IIi")
MAX_ROWS = 2_000_000
MAX_FIELD = 1 << 20


def build_payload(source_bytes: bytes) -> tuple[int, bytes]:
    payload = bytearray()
    count = 0
    for raw in source_bytes.decode("utf-8-sig", errors="strict").splitlines():
        if not raw or raw.startswith(("#", ";")):
            continue
        fields = raw.split("\t")
        if len(fields) < 2 or not fields[0] or not fields[1]:
            continue
        try:
            frequency = int(fields[2]) if len(fields) >= 3 else 1
        except ValueError:
            continue
        pinyin = fields[0].encode("ascii", errors="strict")
        word = fields[1].encode("utf-8", errors="strict")
        if len(pinyin) > MAX_FIELD or len(word) > MAX_FIELD or count >= MAX_ROWS:
            raise ValueError("lexicon cache limit exceeded")
        payload.extend(RECORD.pack(len(pinyin), len(word), frequency))
        payload.extend(pinyin)
        payload.extend(word)
        count += 1
    return count, bytes(payload)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    source = Path(args.source)
    if not source.is_file():
        parser.error(f"missing source dictionary: {source}")
    source_bytes = source.read_bytes()
    count, payload = build_payload(source_bytes)
    header = HEADER.pack(
        MAGIC,
        VERSION,
        count,
        hashlib.sha256(source_bytes).digest(),
        hashlib.sha256(payload).digest(),
    )
    output = Path(args.out)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f"{output.name}.tmp.{os.getpid()}")
    with temporary.open("wb") as stream:
        stream.write(header)
        stream.write(payload)
    temporary.replace(output)
    print(f"rows={count} bytes={output.stat().st_size} -> {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
