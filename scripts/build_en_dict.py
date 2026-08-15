# -*- coding: utf-8 -*-
"""Build the English word lexicon for 财神输入法."""
from pathlib import Path

ROOT = Path(r"E:\shurufa")
OUT = ROOT / "data" / "lexicon" / "en_dict.txt"
sources = [
    ROOT / "tools" / "tmp" / "google-10000-english.txt",
    ROOT / "tools" / "tmp" / "google-10000-english-usa.txt",
]

# optional extra from words_alpha: common-looking 3..12 letter words not already included
ALPHA = ROOT / "tools" / "tmp" / "words_alpha.txt"

words: dict[str, int] = {}

def add(w: str, fre: int):
    w = w.strip().lower()
    if not w or not w.isalpha():
        return
    if len(w) < 2 or len(w) > 20:
        return
    words[w] = max(words.get(w, 0), fre)

rank = 0
for src in sources:
    if not src.exists():
        continue
    for line in src.read_text(encoding="utf-8", errors="ignore").splitlines():
        rank += 1
        # higher rank (earlier) => higher frequency
        fre = max(100, 200000 - rank * 15)
        add(line, fre)

# top slice of alpha dictionary for coverage (length 3-10)
if ALPHA.exists():
    n = 0
    for line in ALPHA.read_text(encoding="utf-8", errors="ignore").splitlines():
        w = line.strip().lower()
        if not w.isalpha() or not (3 <= len(w) <= 10):
            continue
        if w in words:
            continue
        # low base freq so google list stays on top
        add(w,  mon := 800)
        n += 1
        if n >= 25000:
            break

items = sorted(words.items(), key=lambda kv: (-kv[1], kv[0]))
OUT.parent.mkdir(parents=True, exist_ok=True)
with OUT.open("w", encoding="utf-8", newline="\n") as f:
    f.write("# 财神输入法英文词库\n")
    f.write("# 格式: word<TAB>frequency\n")
    f.write("# 编码: UTF-8\n\n")
    for w, fre in items:
        f.write(f"{w}\t{fre}\n")

print(f"en words={len(items)} -> {OUT}")
print("sample", items[:10])
print("hello", words.get("hello"), "world", words.get("world"), "python", words.get("python"))
