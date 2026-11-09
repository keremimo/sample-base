# SPDX-License-Identifier: AGPL-3.0-only
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SynthTests(unittest.TestCase):
    def test_pitch_velocity_polyphony_and_release(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "synth"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "src"), str(ROOT / "tests/synth.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
