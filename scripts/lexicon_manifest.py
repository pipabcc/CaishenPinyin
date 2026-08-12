#!/usr/bin/env python3
"""Generate and validate the Facai Pinyin versioned lexicon manifest."""
from __future__ import annotations
import argparse, hashlib, json, re, sys
from pathlib import Path

COMMENT = ("#", ";")
PINYIN = re.compile(r"^[a-z]+$")
ENGLISH = re.compile(r"^[A-Za-z][A-Za-z'-]*$")
DEFAULT_METADATA = {
    "base_dict.txt": {"source": "rime-ice cn_dicts/base.dict.yaml commit 569ff3bc65dd4aec0a26b33c49c8bbdfa8b5fd57 plus project custom_dict.txt", "license": "GPL-3.0-only"},
    "char_dict.txt": {"source": "project-generated from base_dict.txt with scripts/build_char_dict.py", "license": "GPL-3.0-only"},
    "en_dict.txt": {"source": "project-generated with scripts/build_en_dict.py; see docs/lexicon-governance.md", "license": "NOASSERTION"},
    "base_dict.txt.bin": {"source": "validated derived cache of base_dict.txt", "license": "GPL-3.0-only"},
    "char_dict.txt.bin": {"source": "validated derived cache of char_dict.txt", "license": "GPL-3.0-only"},
    "GPL-3.0.txt": {"source": "rime-ice LICENSE at pinned commit", "license": "GPL-3.0-only"},
}

FILES = (
    ("base_dict.txt", "chinese"),
    ("char_dict.txt", "chinese"),
    ("en_dict.txt", "english"),
    ("base_dict.txt.bin", "binary-cache"),
    ("char_dict.txt.bin", "binary-cache"),
    ("GPL-3.0.txt", "license"),
)

class ValidationError(Exception): pass

def rows(path: Path):
    for number, raw in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        text = raw.strip()
        if text and not text.startswith(COMMENT):
            yield number, text.split("\t")

def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""): h.update(block)
    return h.hexdigest()

def inspect_file(path: Path, kind: str, min_frequency: int, max_frequency: int):
    if kind == "binary-cache":
        return 0, [] if path.stat().st_size > 80 else [f"{path.name}: binary cache is too small"]
    if kind == "license":
        text = path.read_text(encoding="utf-8", errors="strict")
        return 0, [] if "GNU GENERAL PUBLIC LICENSE" in text and len(text) > 30000 else [f"{path.name}: invalid GPL text"]
    errors, seen, count = [], {}, 0
    for number, fields in rows(path):
        count += 1
        expected = 3 if kind == "chinese" else 2
        if len(fields) != expected:
            errors.append(f"{path.name}:{number}: expected {expected} tab-separated columns, got {len(fields)}")
            continue
        fields = [x.strip() for x in fields]
        if kind == "chinese":
            pinyin, word, frequency = fields
            if not PINYIN.fullmatch(pinyin): errors.append(f"{path.name}:{number}: invalid pinyin {pinyin!r}")
            if not word: errors.append(f"{path.name}:{number}: empty word")
            key = (pinyin, word)
        else:
            word, frequency = fields
            if not ENGLISH.fullmatch(word): errors.append(f"{path.name}:{number}: invalid English word {word!r}")
            key = word.lower()
        try:
            value = int(frequency)
            if value < min_frequency or value > max_frequency:
                errors.append(f"{path.name}:{number}: frequency {value} outside [{min_frequency}, {max_frequency}]")
        except ValueError:
            errors.append(f"{path.name}:{number}: invalid integer frequency {frequency!r}")
        if key in seen: errors.append(f"{path.name}:{number}: exact duplicate of line {seen[key]}: {key!r}")
        else: seen[key] = number
    return count, errors

def build(directory: Path, package_id: str, version: str, schema: str, min_frequency: int, max_frequency: int):
    files, errors = [], []
    for name, kind in FILES:
        path = directory / name
        if not path.is_file(): errors.append(f"missing required file: {name}"); continue
        count, file_errors = inspect_file(path, kind, min_frequency, max_frequency)
        errors.extend(file_errors)
        files.append({"path": name, "kind": kind, **DEFAULT_METADATA[name], "entries": count, "sha256": sha256(path)})
    if errors: raise ValidationError("\n".join(errors))
    return {"packageId": package_id, "schemaVersion": schema, "version": version,
            "frequencyPolicy": {"min": min_frequency, "max": max_frequency}, "files": files}

def validate(directory: Path, manifest_path: Path, min_override, max_override):
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    for field in ("packageId", "schemaVersion", "version", "files"):
        if field not in manifest: raise ValidationError(f"manifest missing {field}")
    policy = manifest.get("frequencyPolicy", {})
    low = policy.get("min", 0) if min_override is None else min_override
    high = policy.get("max", 50_000_000) if max_override is None else max_override
    errors = []
    expected = {item["path"]: item for item in manifest["files"]}
    for name, kind in FILES:
        item, path = expected.get(name), directory / name
        if not item: errors.append(f"manifest missing file record: {name}"); continue
        for field in ("source", "license", "entries", "sha256"):
            if field not in item or item[field] in (None, ""): errors.append(f"{name}: missing metadata {field}")
        if not path.is_file(): errors.append(f"missing required file: {name}"); continue
        count, file_errors = inspect_file(path, kind, int(low), int(high)); errors.extend(file_errors)
        if count != item.get("entries"): errors.append(f"{name}: entry count {count} != manifest {item.get('entries')}")
        digest = sha256(path)
        if digest.lower() != str(item.get("sha256", "")).lower(): errors.append(f"{name}: SHA-256 mismatch")
    if errors: raise ValidationError("\n".join(errors))
    return manifest

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("generate", "validate")); parser.add_argument("--dir", default="data/lexicon")
    parser.add_argument("--manifest", default="data/lexicon/manifest.json"); parser.add_argument("--package-id", default="facai-pinyin-system-lexicon")
    parser.add_argument("--version", default="1.1.0"); parser.add_argument("--schema", default="1")
    parser.add_argument("--min-frequency", type=int); parser.add_argument("--max-frequency", type=int)
    args = parser.parse_args(); directory, manifest_path = Path(args.dir), Path(args.manifest)
    try:
        if args.command == "generate":
            result = build(directory, args.package_id, args.version, args.schema,
                           0 if args.min_frequency is None else args.min_frequency,
                           50_000_000 if args.max_frequency is None else args.max_frequency)
            manifest_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            print(f"generated {manifest_path} ({sum(x['entries'] for x in result['files'])} entries)")
        else:
            result = validate(directory, manifest_path, args.min_frequency, args.max_frequency)
            print(f"valid lexicon package {result['packageId']} {result['version']}")
        return 0
    except (OSError, ValueError, json.JSONDecodeError, ValidationError) as exc:
        print(f"LEXICON_VALIDATION_ERROR: {exc}", file=sys.stderr); return 2
if __name__ == "__main__": sys.exit(main())
