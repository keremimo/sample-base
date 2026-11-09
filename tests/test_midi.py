import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MidiTests(unittest.TestCase):
    def test_stream_parser_and_overflow_recovery(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "midi"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "src"), str(ROOT / "tests/midi.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
