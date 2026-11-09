import subprocess
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from device import Device, service_unit


class SwitchingTests(unittest.TestCase):
    def test_recovery_runs_after_every_app_exit(self):
        unit = service_unit("/run/sample-base-release-123abc", 45)
        self.assertIn("ExecStopPost=/bin/systemctl start ac50.service\n", unit)
        self.assertIn("ExecStartPre=/bin/systemctl stop ac50.service\n", unit)
        self.assertIn("RuntimeMaxSec=45\n", unit)
        self.assertIn("Restart=no\n", unit)
        self.assertIn("ExecStart=/run/sample-base-release-123abc/sample-base\n", unit)
        self.assertNotIn("LD_PRELOAD", unit)

    def test_stages_one_binary_and_runs_builtin_self_test(self):
        device = Device()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "build").mkdir()
            (root / "build/sample-base").write_bytes(b"test executable")
            with patch("device.ROOT", root), patch.object(device, "check_identity"), \
                    patch.object(device, "run") as run, patch("device.subprocess.run") as transfer:
                release = device.stage()
        self.assertEqual(transfer.call_count, 1)
        self.assertEqual(transfer.call_args.args[0][-1], "root@" + device.host + ":" + release + "/sample-base")
        self.assertIn(release + "/sample-base --self-test " + release + "/test.wav",
                      [call.args[0] for call in run.call_args_list])

    def test_rejects_unsafe_release_and_session_duration(self):
        for path in ("/usr/bin/app", "/run/sample-base-release-../app", "/run/sample-base-release-a b"):
            with self.assertRaises(ValueError):
                service_unit(path)
        with self.assertRaises(ValueError):
            service_unit("/run/sample-base-release-abc", -1)

    def test_failed_start_restores_existing_stock_service(self):
        device = Device()
        calls = []
        def run(command, **kwargs):
            calls.append(command)
            if command == "systemctl daemon-reload && systemctl start sample-base.service":
                raise RuntimeError("simulated startup failure")
            return subprocess.CompletedProcess([], 0, "", "")
        with patch.object(device, "stage", return_value="/run/sample-base-release-abc"), patch.object(device, "run", side_effect=run):
            with self.assertRaises(RuntimeError):
                device.start()
        self.assertIn("systemctl start ac50.service", calls)
        self.assertIn("systemctl is-active --quiet ac50.service", calls)
        self.assertFalse(any("ac50.service.d" in command for command in calls))

    def test_identity_mismatch_cannot_stop_stock(self):
        device = Device()
        with patch.object(device, "run", return_value=subprocess.CompletedProcess([], 0, "wrong", "")) as run:
            with self.assertRaises(RuntimeError):
                device.stage()
        self.assertFalse(any("stop" in c.args[0] for c in run.call_args_list))
