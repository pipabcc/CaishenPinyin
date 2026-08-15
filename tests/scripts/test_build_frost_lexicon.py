import importlib.util
import sys
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "build_frost_lexicon.py"
SPEC = importlib.util.spec_from_file_location("build_frost_lexicon", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class NormalizeTokenTest(unittest.TestCase):
    def test_normalizes_all_toned_u_umlaut_forms_to_v(self):
        self.assertEqual(MODULE.normalize_token("nǖ nǘ nǚ nǜ nü nu:"), "nv nv nv nv nv nv")

    def test_removes_tone_marks_and_tone_numbers(self):
        self.assertEqual(MODULE.normalize_token("LÜE4"), "lve")
        self.assertEqual(MODULE.normalize_token("shuǐ3"), "shui")


if __name__ == "__main__":
    unittest.main()
