# SPDX-License-Identifier: AGPL-3.0-only
"""The source layers stay apart: see AGENTS.md and docs/APPS.md."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"

# What each folder may include, besides itself and the standard and system headers.
BASE = {"surface", "dsp"}
ALLOWED = {
    "surface": BASE,
    "dsp": BASE,
    "drivers": BASE | {"drivers"},
    "runtime": BASE | {"drivers", "runtime"},
    "demos": BASE | {"drivers", "runtime", "demos"},
    "starter": BASE | {"drivers", "runtime", "starter"},
}
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)


def sources():
    return sorted(p for p in SRC.rglob("*") if p.suffix in (".h", ".cpp"))


class LayoutTests(unittest.TestCase):
    def test_only_main_sits_directly_in_src(self):
        loose = [p.name for p in SRC.iterdir() if p.is_file()]
        self.assertEqual(loose, ["main.cpp"])
        self.assertEqual({p.name for p in SRC.iterdir() if p.is_dir()}, set(ALLOWED))

    def test_includes_name_their_folder_and_exist(self):
        for path in sources():
            for name in INCLUDE.findall(path.read_text()):
                with self.subTest(file=str(path.relative_to(ROOT)), include=name):
                    self.assertIn("/", name, "include the folder too, e.g. \"drivers/audio.h\"")
                    self.assertTrue((SRC / name).is_file())

    def test_each_layer_includes_only_the_layers_below_it(self):
        for path in sources():
            relative = path.relative_to(SRC)
            if len(relative.parts) == 1:
                continue  # main.cpp wires an app to the runtime and may include anything.
            layer = relative.parts[0]
            for name in INCLUDE.findall(path.read_text()):
                with self.subTest(file=str(relative), include=name):
                    self.assertIn(name.split("/")[0], ALLOWED[layer])


if __name__ == "__main__":
    unittest.main()
