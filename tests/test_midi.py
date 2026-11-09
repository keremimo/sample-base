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

    def test_surface_light_map_and_change_updates(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "leds"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "src"), str(ROOT / "tests/leds.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_light_show_scenes_use_every_light_within_hardware_values(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "light-show"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "src"), str(ROOT / "tests/light_show.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
