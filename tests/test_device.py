import os
import shlex
import subprocess
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from device import Device, service_unit


class SwitchingTests(unittest.TestCase):
    def test_recovery_runs_after_every_app_exit(self):
        unit = service_unit("/run/sample-base-release-123abc", 45)
        self.assertIn("ExecStopPost=/bin/sh /run/sample-base-release-123abc/restore-stock.sh\n", unit)
        self.assertIn("ExecStartPre=/bin/systemctl stop ac50.service\n", unit)
        self.assertIn("RuntimeMaxSec=45\n", unit)
        self.assertIn("Restart=no\n", unit)
        self.assertIn("ExecStart=/run/sample-base-release-123abc/sample-base\n", unit)
        self.assertNotIn("LD_PRELOAD", unit)

    def test_stages_app_and_recovery_script_and_runs_self_test(self):
        device = Device()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "build").mkdir()
            (root / "build/sample-base").write_bytes(b"test executable")
            (root / "install").mkdir()
            (root / "install/restore-stock.sh").write_text("#!/bin/sh\nexit 0\n")
            with patch("device.ROOT", root), patch.object(device, "check_identity"), \
                    patch.object(device, "run") as run, patch("device.subprocess.run") as transfer:
                release = device.stage()
        self.assertEqual(transfer.call_count, 2)
        self.assertEqual(transfer.call_args_list[0].args[0][-1], "root@" + device.host + ":" + release + "/sample-base")
        self.assertEqual(transfer.call_args_list[1].args[0][-1], "root@" + device.host + ":" + release + "/restore-stock.sh")
        self.assertIn(release + "/sample-base --self-test " + release + "/test.wav",
                      [call.args[0] for call in run.call_args_list])

    def test_unit_starts_the_chosen_app(self):
        self.assertIn("ExecStart=/run/sample-base-release-abc/sample-base\n", service_unit("/run/sample-base-release-abc"))
        self.assertIn("ExecStart=/run/sample-base-release-abc/sample-base --app starter\n",
                      service_unit("/run/sample-base-release-abc", app="starter"))
        for app in ("", "two words", "a;reboot", "a\nExecStart=/bin/sh", "x" * 33, "dé"):
            with self.assertRaises(ValueError):
                service_unit("/run/sample-base-release-abc", app=app)

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

    def test_start_swaps_apps_without_restoring_stock(self):
        device = Device()
        calls = []
        def run(command, **kwargs):
            calls.append(command)
            return subprocess.CompletedProcess([], 0, "OK frames=0", "")
        with patch.object(device, "stage", return_value="/run/sample-base-release-abc"), patch.object(device, "run", side_effect=run):
            device.start(app="melodee")
        stop = next(command for command in calls if "systemctl stop sample-base.service" in command)
        self.assertIn("ExecStopPost=", stop)
        self.assertLess(stop.index("swap.conf"), stop.index("systemctl stop sample-base.service"))
        self.assertTrue(stop.endswith("rm -f /run/systemd/system/sample-base.service.d/swap.conf"))
        self.assertNotIn("systemctl start ac50.service", calls)

    def test_identity_mismatch_cannot_stop_stock(self):
        device = Device()
        with patch.object(device, "run", return_value=subprocess.CompletedProcess([], 0, "wrong", "")) as run:
            with self.assertRaises(RuntimeError):
                device.stage()
        self.assertFalse(any("stop" in c.args[0] for c in run.call_args_list))


class RecoveryScriptTests(unittest.TestCase):
    def test_restart_is_suppressed_during_shutdown(self):
        source = (Path(__file__).resolve().parents[1] / "install/restore-stock.sh").read_text()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            systemctl = root / "systemctl"
            systemctl.write_text('#!/bin/sh\nif [ "$1" = is-system-running ]; then echo "$FIXTURE_STATE"; exit 0; fi\nprintf "%s\\n" "$*" >> "$FIXTURE_CALLS"\n')
            systemctl.chmod(0o755)
            script = root / "restore.sh"
            script.write_text(source.replace("/bin/systemctl", shlex.quote(str(systemctl))))
            calls = root / "calls"
            for state in ("running", "degraded", "stopping", "offline"):
                calls.unlink(missing_ok=True)
                subprocess.run(["sh", str(script)], check=True,
                               env={**os.environ, "FIXTURE_STATE": state, "FIXTURE_CALLS": str(calls)})
                self.assertEqual(calls.exists(), state in ("running", "degraded"))
                if calls.exists():
                    self.assertEqual(calls.read_text().strip(), "start ac50.service")


    def test_gadget_mask_restored_before_stock(self):
        source = (Path(__file__).resolve().parents[1] / "install/restore-stock.sh").read_text()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            systemctl = root / "systemctl"
            systemctl.write_text('#!/bin/sh\nif [ "$1" = is-system-running ]; then echo running; exit 0; fi\n'
                                 'cat "$FIXTURE_ROOT/configfs/ac50/UDC" "$FIXTURE_ROOT/configfs/ac50/functions/uac2.audio/p_chmask" > "$FIXTURE_ROOT/at-start"\n')
            systemctl.chmod(0o755)
            configfs, saved = root / "configfs", root / "saved"
            function = configfs / "ac50/functions/uac2.audio"
            function.mkdir(parents=True)
            (function / "p_chmask").write_text("262143\n")
            (configfs / "ac50/UDC").write_text("fcc00000.usb\n")
            config = configfs / "ac50/configs/c.1"
            config.mkdir(parents=True)
            for name in ("uac2.audio", "ncm.network"):
                (config / name).symlink_to("../../functions/" + name)
            script = root / "restore.sh"
            script.write_text(source.replace("/bin/systemctl", shlex.quote(str(systemctl)))
                              .replace("/sys/kernel/config/usb_gadget", str(configfs))
                              .replace("/run/sample-base-uac2-p_chmask", str(saved)))
            saved.write_text(str(function) + " 3\n")
            subprocess.run(["sh", str(script)], check=True, env={**os.environ, "FIXTURE_ROOT": temp})
            self.assertEqual((root / "at-start").read_text(), "fcc00000.usb\n3\n")
            for name in ("uac2.audio", "ncm.network"):
                self.assertEqual(os.readlink(config / name), str(configfs / "ac50/functions" / name))
            self.assertFalse(saved.exists())


class SdAutorunTests(unittest.TestCase):
    OWNER = "#!/bin/sh\n# MPC_PLUGINS_SD_START\ntrap '/bin/sh \"$(dirname -- \"$0\")/../MPC-Plugins/start-plugins.sh\" x' EXIT\nset -eu\necho ssh\n"

    def test_check_is_the_salted_digest(self):
        from device import autorun_check
        self.assertEqual(autorun_check("{}", "#!/bin/sh\n"),
                         __import__("hashlib").sha256(b"{}#!/bin/sh\nf7bed1c9-7e90-4731-9194-c996666b4331").hexdigest())

    def test_chaining_keeps_the_owner_script_and_is_repeatable(self):
        from device import chain_autorun, SD_MARKER
        with self.assertRaises(RuntimeError):
            chain_autorun(self.OWNER)  # MPC-Plugins is there: only replaced when asked
        chained = chain_autorun(self.OWNER, remove_plugins=True)
        self.assertTrue(chained.startswith("#!/bin/sh\n" + SD_MARKER + "\ntrap '"))
        self.assertNotIn("MPC-Plugins", chained)
        self.assertTrue(chained.endswith("set -eu\necho ssh\n"))
        self.assertEqual(chain_autorun(chained), chained)
        with self.assertRaises(RuntimeError):
            chain_autorun("echo no shebang\n")

    def test_uninstall_takes_out_only_our_lines(self):
        from device import chain_autorun, without_marker, SD_MARKER
        plain = "#!/bin/sh\necho ssh\n"
        self.assertEqual(without_marker(chain_autorun(plain), SD_MARKER, "sample-base/sd-boot.sh"), plain)
