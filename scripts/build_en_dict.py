#!/usr/bin/env python3
"""Build the deterministic English lexicon used by Caishen Pinyin.

The runtime file stores three columns::

    lookup_key<TAB>display_word<TAB>frequency

``lookup_key`` is a lower-case ASCII lookup key while ``display_word`` keeps
the casing and apostrophes selected by the upstream dictionary.  The importer
understands the Rime ``word<TAB>code<TAB>weight`` format and the old two-column
project format so rebuilding the dictionary is repeatable and backwards
compatible.
"""
from __future__ import annotations

import argparse
import hashlib
import re
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "data" / "lexicon" / "en_dict.txt"
LEGACY_SOURCE = DEFAULT_OUTPUT
DEFAULT_UPSTREAM = ROOT / "tools" / "tmp" / "easy_en.dict.yaml"
GOOGLE_SOURCES = (
    ROOT / "tools" / "tmp" / "google-10000-english.txt",
    ROOT / "tools" / "tmp" / "google-10000-english-usa.txt",
)
ALPHA_SOURCE = ROOT / "tools" / "tmp" / "words_alpha.txt"
UPSTREAM_COMMIT = "54a4a07289412efc54134092c0d945f895a71ed3"
UPSTREAM_URL = (
    "https://raw.githubusercontent.com/BlindingDark/rime-easy-en/"
    f"{UPSTREAM_COMMIT}/easy_en.dict.yaml"
)
UPSTREAM_SHA256 = "4f039026b2746fa9b0d4d7a248cdf866b64609dca2317708f04e9e68ac7d868a"

KEY_RE = re.compile(r"^[A-Za-z]+$")
DISPLAY_RE = re.compile(r"^[A-Za-z][A-Za-z'-]*$")
MIN_KEY_LENGTH = 2
MAX_KEY_LENGTH = 32


@dataclass(frozen=True)
class Entry:
    key: str
    display: str
    frequency: int
    source_priority: int
    display_priority: int


def normalize_key(value: str) -> str:
    """Return a key only when the source is an unambiguous ASCII word code."""
    value = value.strip()
    if not KEY_RE.fullmatch(value):
        return ""
    key = value.lower()
    if not MIN_KEY_LENGTH <= len(key) <= MAX_KEY_LENGTH:
        return ""
    return key


def valid_display(value: str) -> bool:
    return bool(DISPLAY_RE.fullmatch(value.strip()))


def parse_frequency(value: str, default: int = 0) -> int:
    try:
        return max(0, int(value.strip()))
    except (TypeError, ValueError):
        return default


def display_priority(display: str, code: str, key: str) -> tuple[int, int, int, str]:
    """Prefer the canonical row (lower-case code) from Rime's case variants."""
    # Rime emits three case variants for many entries.  The row whose code is
    # lower-case carries the intended spelling: ``English``, ``API`` or
    # ``don't``.  The remaining tie-breakers are deterministic.
    return (
        0 if code == code.lower() else 1,
        0 if display == display.strip() else 1,
        0 if display.lower() == key else 1,
        display,
    )


def add_entry(entries: dict[str, Entry], entry: Entry) -> None:
    current = entries.get(entry.key)
    if current is None:
        entries[entry.key] = entry
        return
    frequency = max(current.frequency, entry.frequency)
    current_display_rank = (
        1 if current.frequency == frequency else 0,
        current.source_priority,
        1 if current.display.lower() == current.key else 0,
        -current.display_priority,
    )
    entry_display_rank = (
        1 if entry.frequency == frequency else 0,
        entry.source_priority,
        1 if entry.display.lower() == entry.key else 0,
        -entry.display_priority,
    )
    display_source = current
    if entry_display_rank > current_display_rank or (
        entry_display_rank == current_display_rank and entry.display < current.display
    ):
        display_source = entry
    entries[entry.key] = Entry(
        key=entry.key,
        display=display_source.display,
        frequency=frequency,
        source_priority=display_source.source_priority,
        display_priority=display_source.display_priority,
    )


def make_entry(
    code: str,
    display: str,
    frequency: int,
    source_priority: int,
) -> Entry | None:
    key = normalize_key(code)
    display = display.strip()
    if not key or not valid_display(display):
        return None
    priority = display_priority(display, code.strip(), key)
    return Entry(
        key=key,
        display=display,
        frequency=max(0, frequency),
        source_priority=source_priority,
        display_priority=priority[0],
    )


def import_existing(path: Path, entries: dict[str, Entry]) -> int:
    """Import the previous project output, including both supported formats."""
    if not path.is_file():
        return 0
    accepted = 0
    for raw in path.read_text(encoding="utf-8-sig", errors="ignore").splitlines():
        line = raw.strip()
        if not line or line.startswith(("#", ";")):
            continue
        fields = line.split("\t")
        if len(fields) == 2:
            code, frequency = fields
            display = code
        elif len(fields) >= 3:
            code, display, frequency = fields[:3]
        else:
            continue
        entry = make_entry(code, display, parse_frequency(frequency), 2)
        if entry is not None:
            add_entry(entries, entry)
            accepted += 1
    return accepted


def import_google(entries: dict[str, Entry]) -> int:
    accepted = 0
    rank = 0
    for source in GOOGLE_SOURCES:
        if not source.is_file():
            continue
        for raw in source.read_text(encoding="utf-8", errors="ignore").splitlines():
            rank += 1
            word = raw.strip()
            entry = make_entry(word, word.lower(), max(100, 200000 - rank * 15), 1)
            if entry is not None:
                add_entry(entries, entry)
                accepted += 1
    return accepted


def import_alpha(entries: dict[str, Entry], limit: int = 25_000) -> int:
    if not ALPHA_SOURCE.is_file():
        return 0
    accepted = 0
    for raw in ALPHA_SOURCE.read_text(encoding="utf-8", errors="ignore").splitlines():
        word = raw.strip().lower()
        entry = make_entry(word, word, 800, 0)
        if entry is None or entry.key in entries:
            continue
        add_entry(entries, entry)
        accepted += 1
        if accepted >= limit:
            break
    return accepted


def import_rime_easy_en(path: Path, entries: dict[str, Entry]) -> int:
    if not path.is_file():
        raise FileNotFoundError(
            f"missing rime-easy-en source: {path}\n"
            f"download {UPSTREAM_URL} to this path or pass --upstream"
        )
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != UPSTREAM_SHA256:
        raise ValueError(
            f"rime-easy-en SHA-256 mismatch: {digest} != {UPSTREAM_SHA256}"
        )
    in_body = False
    accepted = 0
    for raw in path.read_text(encoding="utf-8-sig", errors="ignore").splitlines():
        if not in_body:
            if raw.strip() == "...":
                in_body = True
            continue
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        fields = raw.split("\t")
        if len(fields) < 3:
            # Accent/ligature aliases in the header intentionally have only
            # two columns and are not standalone English candidates.
            continue
        display, code, weight = fields[:3]
        entry = make_entry(code, display, parse_frequency(weight), 3)
        if entry is not None:
            add_entry(entries, entry)
            accepted += 1
    if not in_body:
        raise ValueError(f"Rime dictionary body marker '...' not found: {path}")
    return accepted


def build(output: Path, upstream: Path, legacy_source: Path) -> tuple[int, int, int]:
    entries: dict[str, Entry] = {}
    legacy = import_existing(legacy_source, entries)
    if legacy == 0:
        import_google(entries)
        import_alpha(entries)
    upstream_rows = import_rime_easy_en(upstream, entries)
    items = sorted(
        entries.values(), key=lambda item: (-item.frequency, item.key, item.display)
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write("# 财神输入法英文词库\n")
        stream.write("# 格式: lookup_key<TAB>display_word<TAB>frequency\n")
        stream.write("# 查询键大小写不敏感；display_word 保留规范大小写\n")
        stream.write(f"# rime-easy-en commit: {UPSTREAM_COMMIT}\n")
        stream.write(f"# source: {UPSTREAM_URL}\n\n")
        for item in items:
            stream.write(f"{item.key}\t{item.display}\t{item.frequency}\n")
    return len(items), legacy, upstream_rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--upstream", type=Path, default=DEFAULT_UPSTREAM)
    parser.add_argument("--legacy", type=Path, default=LEGACY_SOURCE)
    args = parser.parse_args()
    count, legacy, upstream_rows = build(args.output, args.upstream, args.legacy)
    print(
        f"en words={count} -> {args.output} "
        f"(legacy={legacy}, rime-easy-en accepted={upstream_rows})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
