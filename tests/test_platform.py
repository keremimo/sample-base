# SPDX-License-Identifier: AGPL-3.0-only
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def compile_fixture(output, *sources, extra=()):
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-I" + str(ROOT / "src"), *(str(ROOT / name) for name in sources), *extra, "-o", str(output)], check=True)


class PlatformTests(unittest.TestCase):
    def test_midi_packets_surface_state_and_platform_files(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "platform"
            compile_fixture(binary, "tests/platform.cpp", "src/drivers/platform.cpp")
            subprocess.run([str(binary), str(Path(temp) / "fixture")], check=True)

    def test_system_page_model(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "system"
            compile_fixture(binary, "tests/system.cpp", "src/drivers/platform.cpp")
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_codec_link_power_clock_and_immediate_power_down(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "codec-power"
            compile_fixture(binary, "tests/codec_power.cpp", "src/drivers/codec_power.cpp")
            subprocess.run([str(binary), temp], check=True, timeout=5)

    @unittest.skipUnless(sys.platform == "linux" and shutil.which("dbus-daemon"), "requires Linux system bus libraries")
    def test_storage_service_and_suspend_on_private_bus(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "services"
            compile_fixture(binary, "tests/system_services.cpp", "src/drivers/system_services.cpp", "src/drivers/platform.cpp", extra=("-lsystemd",))
            daemon = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--print-address=1"],
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                address = daemon.stdout.readline().strip()
                self.assertTrue(address)
                subprocess.run([str(binary)], check=True, timeout=15,
                               env={**os.environ, "DBUS_SYSTEM_BUS_ADDRESS": address})
            finally:
                daemon.terminate()
                daemon.communicate(timeout=5)
