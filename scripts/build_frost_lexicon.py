#!/usr/bin/env python3
"""Convert the pinned Rime Frost dictionaries into deterministic runtime files."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import unicodedata
from dataclasses import asdict, dataclass
from pathlib import Path

FROST_COMMIT = "2aedeea96c1468c1caa17cea01864419a11a4b26"
MAX_FREQUENCY = 50_000_000
PINYIN_TOKEN = re.compile(r"^[a-z]+$")
TONED_U_UMLAUT = str.maketrans({"ǖ": "v", "ǘ": "v", "ǚ": "v", "ǜ": "v"})
DEFAULT_SOURCES = (
    "cn_dicts/8105.dict.yaml",
    "cn_dicts/base.dict.yaml",
    "cn_dicts/ext.dict.yaml",
    "cn_dicts/others.dict.yaml",
    "cn_dicts/corrections.dict.yaml",
    "cn_dicts_cell/exthot.dict.yaml",
    "cn_dicts_cell/idiom.dict.yaml",
    "cn_dicts_cell/name.dict.yaml",
    "cn_dicts_cell/name2.dict.yaml",
    "cn_dicts_cell/place.dict.yaml",
)


@dataclass
class SourceStats:
    path: str
    sha256: str
    data_rows: int = 0
    accepted: int = 0
    invalid: int = 0
    duplicates: int = 0


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def load_syllables(header: Path) -> set[str]:
    text = header.read_text(encoding="utf-8", errors="strict")
    syllables = set(re.findall(r'"([a-z]+)"', text))
    if len(syllables) < 300:
        raise ValueError(f"unexpected pinyin syllable table: {header}")
    return syllables


def is_supported_word(word: str) -> bool:
    return bool(word) and all(0x4E00 <= ord(character) <= 0x9FFF for character in word)


def normalize_token(raw: str) -> str:
    raw = (
        raw.strip()
        .lower()
        .translate(TONED_U_UMLAUT)
        .replace("u:", "v")
        .replace("ü", "v")
    )
    normalized = unicodedata.normalize("NFD", raw)
    output: list[str] = []
    for character in normalized:
        if unicodedata.combining(character):
            continue
        if character in "12345":
            continue
        if character == "ü":
            character = "v"
        output.append(character)
    return "".join(output)


def normalize_spaced_pinyin(raw: str, syllables: set[str]) -> tuple[str, int] | None:
    raw_tokens = re.split(r"[ '\t]+", raw.strip())
    tokens = [normalize_token(token) for token in raw_tokens if token]
    if not tokens or any(
        not PINYIN_TOKEN.fullmatch(token) or token not in syllables
        for token in tokens
    ):
        return None
    return "".join(tokens), len(tokens)


def compact_has_syllable_count(
    compact: str, expected_count: int, syllables: set[str]
) -> bool:
    reachable: list[set[int]] = [set() for _ in range(len(compact) + 1)]
    reachable[0].add(0)
    maximum_length = max(map(len, syllables))
    for begin, counts in enumerate(reachable):
        if not counts:
            continue
        for end in range(begin + 1, min(len(compact), begin + maximum_length) + 1):
            if compact[begin:end] not in syllables:
                continue
            for count in counts:
                if count < expected_count:
                    reachable[end].add(count + 1)
    return expected_count in reachable[-1]


def parse_frequency(raw: str) -> int | None:
    try:
        value = int(float(raw.strip()))
    except ValueError:
        return None
    return min(MAX_FREQUENCY, max(0, value))


def merge_entry(
    target: dict[tuple[str, str], int],
    key: tuple[str, str],
    frequency: int,
) -> bool:
    previous = target.get(key)
    if previous is None:
        target[key] = frequency
        return False
    if frequency > previous:
        target[key] = frequency
    return True


def read_rime_source(
    path: Path,
    relative_path: str,
    syllables: set[str],
    base: dict[tuple[str, str], int],
    chars: dict[tuple[str, str], int],
    invalid_examples: list[str],
) -> SourceStats:
    stats = SourceStats(relative_path, sha256(path))
    in_data = False
    with path.open(encoding="utf-8-sig", errors="strict") as source:
        for line_number, raw in enumerate(source, 1):
            text = raw.rstrip("\r\n")
            if not in_data:
                if text.strip() == "...":
                    in_data = True
                continue
            stripped = text.strip()
            if not stripped or stripped.startswith("#"):
                continue
            stats.data_rows += 1
            fields = text.split("\t")
            if len(fields) < 2:
                stats.invalid += 1
                if len(invalid_examples) < 50:
                    invalid_examples.append(f"{relative_path}:{line_number}: missing columns")
                continue
            word = fields[0].strip()
            normalized = normalize_spaced_pinyin(fields[1], syllables)
            frequency = parse_frequency(fields[2]) if len(fields) >= 3 else 1
            if (
                not is_supported_word(word)
                or normalized is None
                or normalized[1] != len(word)
                or frequency is None
            ):
                stats.invalid += 1
                if len(invalid_examples) < 50:
                    invalid_examples.append(
                        f"{relative_path}:{line_number}: unsupported word/pinyin/weight"
                    )
                continue
            compact, _ = normalized
            target = chars if len(word) == 1 else base
            if merge_entry(target, (compact, word), frequency):
                stats.duplicates += 1
            stats.accepted += 1
    if not in_data:
        raise ValueError(f"Rime data marker missing: {path}")
    return stats


def read_custom_source(
    path: Path,
    syllables: set[str],
    base: dict[tuple[str, str], int],
    chars: dict[tuple[str, str], int],
    invalid_examples: list[str],
) -> SourceStats:
    stats = SourceStats(path.name, sha256(path))
    with path.open(encoding="utf-8-sig", errors="strict") as source:
        for line_number, raw in enumerate(source, 1):
            stripped = raw.strip()
            if not stripped or stripped.startswith(("#", ";")):
                continue
            stats.data_rows += 1
            fields = stripped.split("\t")
            if len(fields) != 3:
                stats.invalid += 1
                continue
            compact = normalize_token(fields[0])
            word = fields[1].strip()
            frequency = parse_frequency(fields[2])
            if (
                not PINYIN_TOKEN.fullmatch(compact)
                or not is_supported_word(word)
                or frequency is None
                or not compact_has_syllable_count(compact, len(word), syllables)
            ):
                stats.invalid += 1
                if len(invalid_examples) < 50:
                    invalid_examples.append(
                        f"{path.name}:{line_number}: unsupported custom entry"
                    )
                continue
            target = chars if len(word) == 1 else base
            if merge_entry(target, (compact, word), frequency):
                stats.duplicates += 1
            stats.accepted += 1
    return stats


def write_dictionary(
    path: Path,
    title: str,
    entries: dict[tuple[str, str], int],
) -> None:
    rows = sorted(
        ((pinyin, word, frequency) for (pinyin, word), frequency in entries.items()),
        key=lambda row: (row[0], -row[2], row[1]),
    )
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as output:
        output.write(f"# {title}\n")
        output.write(f"# Rime Frost commit: {FROST_COMMIT}\n")
        output.write("# Format: pinyin<TAB>word<TAB>frequency\n\n")
        for pinyin, word, frequency in rows:
            output.write(f"{pinyin}\t{word}\t{frequency}\n")
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--frost-root", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--syllable-header", default="src/engine/pinyin_syllables.h")
    parser.add_argument("--custom", action="append", default=[])
    parser.add_argument("--report", default="")
    args = parser.parse_args()

    frost_root = Path(args.frost_root)
    output_dir = Path(args.output_dir)
    syllables = load_syllables(Path(args.syllable_header))
    base: dict[tuple[str, str], int] = {}
    chars: dict[tuple[str, str], int] = {}
    invalid_examples: list[str] = []
    stats: list[SourceStats] = []

    for relative_path in DEFAULT_SOURCES:
        source_path = frost_root / Path(relative_path)
        if not source_path.is_file():
            parser.error(f"missing Rime Frost source: {source_path}")
        stats.append(
            read_rime_source(
                source_path,
                relative_path,
                syllables,
                base,
                chars,
                invalid_examples,
            )
        )
    for custom_path_text in args.custom:
        custom_path = Path(custom_path_text)
        if not custom_path.is_file():
            parser.error(f"missing custom dictionary: {custom_path}")
        stats.append(
            read_custom_source(
                custom_path, syllables, base, chars, invalid_examples
            )
        )

    if not base or not chars:
        parser.error("no dictionary records generated")
    output_dir.mkdir(parents=True, exist_ok=True)
    write_dictionary(
        output_dir / "base_dict.txt",
        "Caishen Pinyin system lexicon derived from Rime Frost",
        base,
    )
    write_dictionary(
        output_dir / "char_dict.txt",
        "Caishen Pinyin character lexicon derived from Rime Frost",
        chars,
    )

    report = {
        "upstream": "https://github.com/gaboolic/rime-frost",
        "commit": FROST_COMMIT,
        "baseEntries": len(base),
        "characterEntries": len(chars),
        "sources": [asdict(item) for item in stats],
        "invalidExamples": invalid_examples,
    }
    if args.report:
        report_path = Path(args.report)
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
    print(
        f"base={len(base)} chars={len(chars)} "
        f"invalid={sum(item.invalid for item in stats)} "
        f"duplicates={sum(item.duplicates for item in stats)}"
    )
    for example in invalid_examples:
        print(f"skip: {example}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
