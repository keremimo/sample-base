# SPDX-License-Identifier: AGPL-3.0-only
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RecorderTests(unittest.TestCase):
    def test_channel_selection_formats_resampling_ring_and_takes(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "recorder"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "src"), str(ROOT / "tests/recorder.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)
