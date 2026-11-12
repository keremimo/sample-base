# SPDX-License-Identifier: AGPL-3.0-only
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_platform import compile_fixture


class DisplayTests(unittest.TestCase):
    def test_damage_plan_sends_a_moving_line_a_column_at_a_time(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "display_damage"
            compile_fixture(binary, "tests/display_damage.cpp")
            result = subprocess.run([str(binary)], capture_output=True, text=True, check=True)
            self.assertIn("display damage ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
