import struct
import tempfile
import unittest
from pathlib import Path

from scripts import lexicon_manifest


class GrammarModelValidationTest(unittest.TestCase):
    def write_grammar(self, path: Path, *, format_name=b"Rime::Grammar/1.0"):
        unit_count = 256
        metadata = lexicon_manifest.GRAMMAR_METADATA.pack(
            format_name.ljust(32, b"\0"), 0, unit_count, 4
        )
        units = bytearray(unit_count * 4)
        struct.pack_into("<I", units, 0, 1 << 10)
        path.write_bytes(metadata + units)

    def test_accepts_structurally_valid_grammar(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "valid.gram"
            self.write_grammar(path)
            count, errors = lexicon_manifest.inspect_grammar_model(path)
            self.assertEqual(count, 256)
            self.assertEqual(errors, [])

    def test_rejects_bad_format_and_truncation(self):
        with tempfile.TemporaryDirectory() as directory:
            bad_format = Path(directory) / "bad-format.gram"
            self.write_grammar(bad_format, format_name=b"Not Grammar")
            self.assertTrue(
                lexicon_manifest.inspect_grammar_model(bad_format)[1]
            )

            truncated = Path(directory) / "truncated.gram"
            self.write_grammar(truncated)
            truncated.write_bytes(truncated.read_bytes()[:-4])
            self.assertTrue(
                lexicon_manifest.inspect_grammar_model(truncated)[1]
            )


class LegacyNgramValidationTest(unittest.TestCase):
    def write_ngram(self, path: Path):
        bigram_key = (ord("北") << 16) | ord("京")
        trigram_key = (ord("去") << 32) | (ord("北") << 16) | ord("京")
        path.write_bytes(
            lexicon_manifest.NGRAM_HEADER.pack(
                lexicon_manifest.NGRAM_MAGIC,
                lexicon_manifest.NGRAM_VERSION,
                1,
                1,
            )
            + lexicon_manifest.NGRAM_BIGRAM.pack(bigram_key, 20)
            + lexicon_manifest.NGRAM_TRIGRAM.pack(trigram_key, 10)
        )

    def test_accepts_structurally_valid_ngram(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "valid.bin"
            self.write_ngram(path)
            count, errors = lexicon_manifest.inspect_legacy_ngram_model(path)
            self.assertEqual(count, 2)
            self.assertEqual(errors, [])

    def test_rejects_corrupt_and_truncated_ngram(self):
        with tempfile.TemporaryDirectory() as directory:
            corrupt = Path(directory) / "corrupt.bin"
            self.write_ngram(corrupt)
            payload = bytearray(corrupt.read_bytes())
            payload[0] = ord("X")
            corrupt.write_bytes(payload)
            self.assertTrue(
                lexicon_manifest.inspect_legacy_ngram_model(corrupt)[1]
            )

            truncated = Path(directory) / "truncated.bin"
            self.write_ngram(truncated)
            truncated.write_bytes(truncated.read_bytes()[:-1])
            self.assertTrue(
                lexicon_manifest.inspect_legacy_ngram_model(truncated)[1]
            )


if __name__ == "__main__":
    unittest.main()
