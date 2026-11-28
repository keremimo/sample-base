# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
import os
import re
import shlex
import subprocess
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from device import Device, service_unit, stale_releases

ROOT = Path(__file__).resolve().parents[1]

ACTIVE_RELEASE = "/run/sample-base-release-abcdef012345"
RUN_NAMES = ["sample-base", "sample-base-release-abcdef012345", "sample-base-release-2e98caf725ed",
             "sample-base-release-b62e9b609711", "sample-base-release-sdadc1aeff3b4d", "sample-base-sd-started",
             "sample-base-sd.log", "sample-base-uac2-p_chmask", "systemd"]
DF = "Filesystem 1024-blocks Used Available Capacity Mounted on\ntmpfs 1009008 144400 {} 14% /run\n"


def fake_mpc(calls, free_kb=864608, active=ACTIVE_RELEASE, fail=None):
    """A run() that answers like the MPC and records each command."""
    def run(command, **kwargs):
        calls.append(command)
        if command == fail:
            raise RuntimeError("simulated failure")
        out = {"df -Pk /run": DF.format(free_kb), "readlink /run/sample-base": active + "\n",
               "ls -1A /run": "\n".join(RUN_NAMES) + "\n"}.get(command, "OK frames=0")
        return subprocess.CompletedProcess([], 0, out, "")
    return run


class ReleaseCleanupTests(unittest.TestCase):
    def test_releases_other_than_the_active_one(self):
        self.assertEqual(stale_releases(RUN_NAMES, ACTIVE_RELEASE),
                         ["/run/sample-base-release-2e98caf725ed", "/run/sample-base-release-b62e9b609711",
                          "/run/sample-base-release-sdadc1aeff3b4d"])

    def test_near_misses_are_kept(self):
        names = ["sample-base-release-sd0123456789",     # SD, 12 characters after the prefix
                 "sample-base-release-sdabcdef0123456", "sample-base-release-SDadc1aeff3b4d",
                 "sample-base-release-ABCDEF012345", "sample-base-release-abcdef01234",
                 "sample-base-release-abcdef0123456", "sample-base-release-abcdef01234g",
                 "sample-base-release-abcdef012345.old", " sample-base-release-abcdef012345",
                 "sample-base-release-abcdef012345 ", "x-sample-base-release-abcdef012345",
                 "sample-base-release-", "sample-base-release-*", "sample-base-release-$HOME000000",
                 "sample-base-release-٠١٢٣٤٥٦٧٨٩ab", "sample-base", "sample-base.next"]
        self.assertEqual(stale_releases(names, ACTIVE_RELEASE), [])

    def test_sd_release_kept_while_active(self):
        self.assertEqual(stale_releases(RUN_NAMES, "/run/sample-base-release-sdadc1aeff3b4d"),
                         ["/run/sample-base-release-2e98caf725ed", "/run/sample-base-release-abcdef012345",
                          "/run/sample-base-release-b62e9b609711"])

    def test_no_active_release_removes_nothing(self):
        for active in ("", "/run/other", "/run/sample-base-release-", "/run/sample-base-release-../x"):
            with self.assertRaises(ValueError):
                stale_releases(RUN_NAMES, active)

    def test_start_removes_stale_releases_after_the_app_is_ready(self):
        device, calls = Device(), []
        with patch.object(device, "stage", return_value=ACTIVE_RELEASE), \
                patch.object(device, "run", side_effect=fake_mpc(calls)):
            device.start(app="melodee")
        removals = [command for command in calls if command.startswith("rm ")]
        self.assertEqual(removals, ["rm -r /run/sample-base-release-2e98caf725ed /run/sample-base-release-b62e9b609711 /run/sample-base-release-sdadc1aeff3b4d"])
        self.assertGreater(calls.index(removals[0]), calls.index("/run/sample-base/sample-base --ctl status"))

    def test_cleanup_skipped_when_active_link_is_elsewhere(self):
        device, calls = Device(), []
        with patch.object(device, "stage", return_value="/run/sample-base-release-111111111111"), \
                patch.object(device, "run", side_effect=fake_mpc(calls)):
            device.start()
        self.assertFalse(any(command.startswith("rm ") for command in calls))

    def test_failed_cleanup_keeps_the_app_running(self):
        device, calls = Device(), []
        failing = "rm -r /run/sample-base-release-2e98caf725ed /run/sample-base-release-b62e9b609711 /run/sample-base-release-sdadc1aeff3b4d"
        with patch.object(device, "stage", return_value=ACTIVE_RELEASE), \
                patch.object(device, "run", side_effect=fake_mpc(calls, fail=failing)):
            device.start()
        self.assertIn(failing, calls)
        self.assertNotIn("systemctl start ac50.service", calls)

    def test_stage_refuses_when_run_is_low(self):
        device, calls = Device(), []
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "build").mkdir()
            (root / "build/sample-base").write_bytes(b"x" * (20 << 20))
            (root / "install").mkdir()
            (root / "install/restore-stock.sh").write_text("#!/bin/sh\nexit 0\n")
            (root / "install/sd-update.sh").write_text("#!/bin/sh\nexit 0\n")
            with patch("device.ROOT", root), patch("device.BINARY", root / "build/sample-base"), \
                    patch.object(device, "check_identity"), \
                    patch.object(device, "run", side_effect=fake_mpc(calls, free_kb=40 << 10)), \
                    patch("device.subprocess.run") as transfer:
                with self.assertRaises(RuntimeError) as raised:
                    device.stage()
        self.assertIn("Only 40 MB free on the MPC's /run", str(raised.exception))
        self.assertIn("rm -r /run/sample-base-release-2e98caf725ed /run/sample-base-release-b62e9b609711 /run/sample-base-release-sdadc1aeff3b4d",
                      str(raised.exception))
        self.assertFalse(any(command.startswith(("mkdir", "rm")) for command in calls))
        transfer.assert_not_called()


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
        device = Device(port=2222)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "build").mkdir()
            (root / "build/sample-base").write_bytes(b"test executable")
            (root / "install").mkdir()
            (root / "install/restore-stock.sh").write_text("#!/bin/sh\nexit 0\n")
            (root / "install/sd-update.sh").write_text("#!/bin/sh\nexit 0\n")
            with patch("device.ROOT", root), patch("device.BINARY", root / "build/sample-base"), \
                    patch.object(device, "check_identity"), \
                    patch.object(device, "run", side_effect=fake_mpc([])) as run, \
                    patch("device.subprocess.run") as transfer:
                release = device.stage()
        self.assertEqual(transfer.call_count, 3)
        self.assertEqual(transfer.call_args_list[0].args[0][-1], "root@" + device.host + ":" + release + "/sample-base")
        self.assertEqual(transfer.call_args_list[1].args[0][-1], "root@" + device.host + ":" + release + "/restore-stock.sh")
        self.assertEqual(transfer.call_args_list[2].args[0][-1], "root@" + device.host + ":" + release + "/sd-update.sh")
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
        self.assertFalse(any(command.startswith("rm ") for command in calls))

    def test_start_swaps_apps_without_restoring_stock(self):
        device = Device()
        calls = []
        def run(command, **kwargs):
            calls.append(command)
            return subprocess.CompletedProcess([], 0, "OK frames=0", "")
        with patch.object(device, "stage", return_value="/run/sample-base-release-abc"), patch.object(device, "run", side_effect=run):
            device.start(app="starter")
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
                    self.assertEqual(calls.read_text().strip().split("\n")[-2:], ["start az0x-webserver.service", "start ac50.service"])


    def test_panel_given_back_to_stock_driver_before_stock(self):
        source = (Path(__file__).resolve().parents[1] / "install/restore-stock.sh").read_text()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            systemctl = root / "systemctl"
            systemctl.write_text('#!/bin/sh\nif [ "$1" = is-system-running ]; then echo running; exit 0; fi\n'
                                 'cat "$FIXTURE_ROOT/spi/drivers/st7796s/bind" > "$FIXTURE_ROOT/at-start"\n')
            rmmod = root / "rmmod"
            rmmod.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$FIXTURE_ROOT/removed"\n')
            for tool in (systemctl, rmmod):
                tool.chmod(0o755)
            spi = root / "spi"
            device = spi / "devices/spi3.0"
            device.mkdir(parents=True)
            (device / "driver_override").write_text("sample-base-panel\n")
            for driver in ("sample-base-panel", "st7796s"):
                (spi / "drivers" / driver).mkdir(parents=True)
                for name in ("bind", "unbind"):
                    (spi / "drivers" / driver / name).write_text("")
            script = root / "restore.sh"
            script.write_text(source.replace("/bin/systemctl", shlex.quote(str(systemctl)))
                              .replace("/sys/bus/spi", str(spi)).replace("rmmod ", shlex.quote(str(rmmod)) + " ")
                              .replace("/sys/kernel/config/usb_gadget", str(root / "none")))
            subprocess.run(["sh", str(script)], check=True, env={**os.environ, "FIXTURE_ROOT": temp})
            self.assertEqual((spi / "drivers/sample-base-panel/unbind").read_text(), "spi3.0\n")
            self.assertEqual((device / "driver_override").read_text(), "\n")
            self.assertEqual((root / "at-start").read_text(), "spi3.0\n")
            self.assertEqual((root / "removed").read_text().splitlines()[-1], "sample_base_panel") # the last module out

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

    def test_screen_mirror_webcam_taken_out_before_stock(self):
        source = (Path(__file__).resolve().parents[1] / "install/restore-stock.sh").read_text()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            systemctl = root / "systemctl"
            systemctl.write_text('#!/bin/sh\nif [ "$1" = is-system-running ]; then echo running; exit 0; fi\n'
                                 'cat "$FIXTURE_ROOT/configfs/ac50/UDC" > "$FIXTURE_ROOT/at-start"\n')
            rmmod = root / "rmmod"
            rmmod.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$FIXTURE_ROOT/removed"\n')
            for tool in (systemctl, rmmod):
                tool.chmod(0o755)
            configfs = root / "configfs"
            webcam = configfs / "ac50/functions/uvc.screen"
            for folder in ("control/header/h", "control/class/fs", "streaming/header/h", "streaming/mjpeg/m/f", "streaming/class/hs"):
                (webcam / folder).mkdir(parents=True)
            (webcam / "streaming/mjpeg/m/f/wWidth").write_text("1280\n")
            (webcam / "control/class/fs/h").symlink_to(webcam / "control/header/h")
            (webcam / "streaming/class/hs/h").symlink_to(webcam / "streaming/header/h")
            (webcam / "streaming/header/h/m").symlink_to(webcam / "streaming/mjpeg/m")
            (configfs / "ac50/functions/uac2.audio").mkdir(parents=True)
            (configfs / "ac50/UDC").write_text("fcc00000.usb\n")
            config = configfs / "ac50/configs/c.1"
            config.mkdir(parents=True)
            for name in ("uac2.audio", "uvc.screen"):
                (config / name).symlink_to(configfs / "ac50/functions" / name)
            script = root / "restore.sh"
            script.write_text(source.replace("/bin/systemctl", shlex.quote(str(systemctl)))
                              .replace("/sys/kernel/config/usb_gadget", str(configfs)).replace("rmmod ", shlex.quote(str(rmmod)) + " ")
                              .replace("/sys/bus/spi", str(root / "none")).replace("/run/sample-base-mirror-live", str(root / "live")))
            # A computer showing the camera: it stays (its app may not survive it unplugged).
            (root / "live").write_text("")
            subprocess.run(["sh", str(script)], check=True, env={**os.environ, "FIXTURE_ROOT": temp})
            self.assertTrue((config / "uvc.screen").is_symlink() and (webcam / "streaming/header/h/m").is_symlink())
            (root / "live").unlink()
            subprocess.run(["sh", str(script)], check=True, env={**os.environ, "FIXTURE_ROOT": temp})
            self.assertFalse(os.path.lexists(config / "uvc.screen"))
            self.assertTrue((config / "uac2.audio").is_symlink())
            # A plain folder keeps the attribute file configfs would drop with the function; the links and the
            # folders made for it are gone.
            self.assertFalse((webcam / "streaming/header/h").exists() or (webcam / "control/class/fs/h").is_symlink())
            self.assertEqual((root / "at-start").read_text(), "fcc00000.usb\n")
            self.assertIn("sample_base_uvc", (root / "removed").read_text().splitlines())

    def test_gadget_rate_restored_before_stock(self):
        source = (Path(__file__).resolve().parents[1] / "install/restore-stock.sh").read_text()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            systemctl = root / "systemctl"
            systemctl.write_text('#!/bin/sh\nif [ "$1" = is-system-running ]; then echo running; exit 0; fi\n')
            systemctl.chmod(0o755)
            configfs, saved = root / "configfs", root / "saved"
            function = configfs / "ac50/functions/uac2.audio"
            function.mkdir(parents=True)
            (function / "p_chmask").write_text("3\n")
            for name in ("p_srate", "c_srate"):
                (function / name).write_text("96000\n")
            (configfs / "ac50/UDC").write_text("fcc00000.usb\n")
            config = configfs / "ac50/configs/c.1"
            config.mkdir(parents=True)
            (config / "uac2.audio").symlink_to("../../functions/uac2.audio")
            script = root / "restore.sh"
            script.write_text(source.replace("/bin/systemctl", shlex.quote(str(systemctl)))
                              .replace("/sys/kernel/config/usb_gadget", str(configfs))
                              .replace("/run/sample-base-uac2-p_chmask", str(saved)))
            # The mask was never widened, only the rate changed; an older line with no rate means 44.1 kHz.
            for line in (str(function) + " 3 44100\n", str(function) + " 3\n"):
                for name in ("p_srate", "c_srate"):
                    (function / name).write_text("96000\n")
                saved.write_text(line)
                subprocess.run(["sh", str(script)], check=True, env={**os.environ, "FIXTURE_ROOT": temp})
                for name in ("p_srate", "c_srate"):
                    self.assertEqual((function / name).read_text(), "44100\n")
                self.assertEqual(os.readlink(config / "uac2.audio"), str(function))
                self.assertFalse(saved.exists())

    def test_lent_card_taken_back_before_stock(self):
        source = (Path(__file__).resolve().parents[1] / "install/restore-stock.sh").read_text()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            systemctl = root / "systemctl"
            systemctl.write_text('#!/bin/sh\nif [ "$1" = is-system-running ]; then echo running; exit 0; fi\n')
            systemctl.chmod(0o755)
            busctl = root / "bin/busctl"
            busctl.parent.mkdir()
            busctl.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$FIXTURE_ROOT/busctl"\n')
            busctl.chmod(0o755)
            ln = root / "bin/ln"
            ln.write_text('#!/bin/sh\nprintf "%s\\n" "${3##*/}" >> "$FIXTURE_ROOT/ln"\nexec /bin/ln "$@"\n')
            ln.chmod(0o755)
            configfs, saved, sysfs = root / "configfs", root / "card-share", root / "sys"
            function = configfs / "ac50/functions/mass_storage.sd"
            function.mkdir(parents=True)
            (configfs / "ac50/UDC").write_text("fcc00000.usb\n")
            config = configfs / "ac50/configs/c.1"
            config.mkdir(parents=True)
            (config / "mass_storage.sd").symlink_to(function)
            for name in ("uac2.audio", "ncm.network", "midi.midi"):
                (configfs / "ac50/functions" / name).mkdir()
            for name in ("uac2.audio", "midi.midi", "ncm.network"): # as lent: the network after the drive
                (config / name).symlink_to(configfs / "ac50/functions" / name)
            (sysfs / "block/mmcblk1/mmcblk1p1").mkdir(parents=True)
            saved.write_text("mmcblk1 uac2.audio ncm.network midi.midi\n")
            script = root / "restore.sh"
            script.write_text(source.replace("/bin/systemctl", shlex.quote(str(systemctl)))
                              .replace("/sys/kernel/config/usb_gadget", str(configfs))
                              .replace("/run/sample-base-card-share", str(saved))
                              .replace('[ -b "/dev/$DISK" ]', "true")
                              .replace("/sys/block", str(sysfs / "block")))
            subprocess.run(["sh", str(script)], check=True,
                           env={**os.environ, "FIXTURE_ROOT": temp, "PATH": str(busctl.parent) + ":" + os.environ["PATH"]})
            self.assertFalse(os.path.lexists(config / "mass_storage.sd"))
            self.assertFalse(function.exists())
            self.assertFalse(saved.exists())
            self.assertEqual((configfs / "ac50/UDC").read_text(), "fcc00000.usb\n")
            # Linked again in the saved order (the order the controller hands endpoints out in).
            self.assertEqual((root / "ln").read_text().split(), ["uac2.audio", "ncm.network", "midi.midi"])
            for name in ("uac2.audio", "ncm.network", "midi.midi"):
                self.assertEqual(os.readlink(config / name), str(configfs / "ac50/functions" / name))
            self.assertIn("/io/github/inmusicdev/edisksd/dev/mmcblk1p1 io.github.inmusicdev.edisksd.Filesystem Mount",
                          (root / "busctl").read_text())


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

    def test_start_line_reads_the_folder_beside_the_autorun(self):
        from device import chain_autorun, SD_COMMAND, SD_FOLDER, SD_MARKER
        self.assertEqual(SD_FOLDER, "autorun/sample-base")
        self.assertIn('"$(dirname -- "$0")/sample-base/sd-boot.sh"', SD_COMMAND)
        self.assertNotIn("..", SD_COMMAND)
        # An install from before (its folder at the card's top): its line replaced by the new one.
        old = ("#!/bin/sh\n" + SD_MARKER + "\ntrap '/bin/sh \"$(dirname -- \"$0\")/../sample-base/sd-boot.sh\" "
               "\"$(dirname -- \"$0\")/../sample-base\" > /run/sample-base-sd.log 2>&1 || true' EXIT\necho ssh\n")
        chained = chain_autorun(old)
        self.assertNotIn("../sample-base", chained)
        self.assertEqual(chained, chain_autorun("#!/bin/sh\necho ssh\n"))

    def local_device(self):
        def run(command, *, input=None, check=True):
            result = subprocess.run(["sh", "-c", command], input=input, text=True, capture_output=True)
            if check and result.returncode:
                raise RuntimeError(result.stderr.strip())
            return result
        device = Device()
        device.run = run
        return device

    def test_old_folder_at_the_card_top_moves_into_autorun(self):
        with tempfile.TemporaryDirectory() as temp:
            card = Path(temp)
            (card / "autorun").mkdir()
            (card / "sample-base/autorun-backup").mkdir(parents=True)
            (card / "sample-base/autorun-backup/autorun.sh").write_text("original\n")
            (card / "sample-base/sd-boot.sh").write_text("x")
            (card / "sample-base/dev-previous").mkdir()
            with patch("builtins.print"):
                self.local_device().move_old_sd_folder(str(card / "sample-base"), str(card / "autorun/sample-base"))
            self.assertFalse((card / "sample-base").exists())
            self.assertEqual((card / "autorun/sample-base/autorun-backup/autorun.sh").read_text(), "original\n")
            self.assertTrue((card / "autorun/sample-base/dev-previous").is_dir())

    def test_old_folder_beside_a_new_one_gives_its_backup_and_goes(self):
        with tempfile.TemporaryDirectory() as temp:
            card = Path(temp)
            (card / "autorun/sample-base").mkdir(parents=True)
            (card / "sample-base/autorun-backup").mkdir(parents=True)
            (card / "sample-base/autorun-backup/autorun.sh").write_text("original\n")
            for name in ("sample-base", "sd-boot.sh", "SHA256SUMS", "app"):
                (card / "sample-base" / name).write_text("x")
            device = self.local_device()
            with patch("builtins.print"):
                device.move_old_sd_folder(str(card / "sample-base"), str(card / "autorun/sample-base"))
            self.assertFalse((card / "sample-base").exists())
            self.assertEqual((card / "autorun/sample-base/autorun-backup/autorun.sh").read_text(), "original\n")
            # Files install-sd did not put there keep the old folder.
            (card / "sample-base").mkdir()
            (card / "sample-base/notes.txt").write_text("mine")
            (card / "sample-base/app").write_text("x")
            with patch("builtins.print"):
                device.move_old_sd_folder(str(card / "sample-base"), str(card / "autorun/sample-base"))
            self.assertEqual(sorted(os.listdir(card / "sample-base")), ["notes.txt"])
            # No old folder: nothing happens.
            device.move_old_sd_folder(str(card / "nothing"), str(card / "autorun/sample-base"))

    def test_firmware_is_named_by_the_stock_app_digest(self):
        device = Device(port=22)
        for digest, version in (("b6584cba76e416e432ad202509099d34373dc2c3d6a8fb625f5678bf0193b508", "1.3.0.12"),
                                ("2271252ffded26830546d4ee1852e39a2c1f4c574a40c47fba10b5a397727ec2", "1.4.0.7"),
                                ("473fb7260e597e61cc55d9d30595eb9d9aa1304ff1b88960e633ad19457dc91a", "1.4.0.8")):
            answers = {"uname -m": "aarch64\n", "sha256sum '/usr/bin/MPC Sample'": digest + "  /usr/bin/MPC Sample\n"}
            device.run = lambda command, **kwargs: subprocess.CompletedProcess([], 0, answers[command], "")
            self.assertEqual(device.firmware(), version)
        answers["sha256sum '/usr/bin/MPC Sample'"] = "0" * 64 + "  /usr/bin/MPC Sample\n"
        with self.assertRaises(RuntimeError):
            device.firmware()

    def test_card_start_knows_the_same_firmwares(self):
        from device import STOCK_FIRMWARES
        worker = (ROOT / "install/sd-worker.sh").read_text()
        listed = set(re.findall(r"^\s+([0-9a-f]{64})\) ;;$", worker, re.MULTILINE))
        self.assertEqual(listed, set(STOCK_FIRMWARES))

    def card_install(self, card, firmware):
        """install-sd onto a card folder on this computer, as if the MPC ran the given firmware."""
        build = card.parent / "build"
        build.mkdir()
        (build / "sample-base").write_bytes(b"test executable")
        device = self.local_device()
        device.card = lambda: str(card)
        device.firmware = lambda: firmware
        device.copy = lambda source, target: subprocess.run(["cp", str(source), target], check=True)
        with patch("device.BINARY", build / "sample-base"), patch("builtins.print"):
            device.install_sd("melodee")
        return device

    def test_install_on_a_card_without_autorun_on_firmware_1_4(self):
        from device import autorun_check, SD_MARKER, TOP_AUTORUN
        with tempfile.TemporaryDirectory() as temp:
            card = Path(temp) / "card"
            card.mkdir()
            device = self.card_install(card, "1.4.0.7")
            script = (card / "autorun/autorun.sh").read_text()
            manifest = (card / "autorun/autorun.json").read_text()
            self.assertTrue(script.startswith("#!/bin/sh\n" + SD_MARKER + "\n"))
            self.assertEqual((card / "autorun/autorun.check").read_text(), autorun_check(manifest, script))
            self.assertEqual((card / "autorun.sh").read_text(), TOP_AUTORUN.read_text())
            self.assertEqual((card / "autorun/sample-base/app").read_text(), "melodee\n")
            self.assertFalse((card / "autorun/sample-base/autorun-backup/autorun.sh").exists())
            with patch("builtins.print"):
                device.uninstall_sd()
            self.assertFalse((card / "autorun.sh").exists())
            self.assertNotIn(SD_MARKER, (card / "autorun/autorun.sh").read_text())

    def test_firmware_1_3_cards_get_no_top_autorun(self):
        with tempfile.TemporaryDirectory() as temp:
            card = Path(temp) / "card"
            card.mkdir()
            self.card_install(card, "1.3.0.12")
            self.assertTrue((card / "autorun/autorun.sh").is_file())
            self.assertFalse((card / "autorun.sh").exists())

    def test_another_top_autorun_is_left_alone(self):
        with tempfile.TemporaryDirectory() as temp:
            card = Path(temp) / "card"
            card.mkdir()
            (card / "autorun.sh").write_text("#!/bin/sh\necho someone else\n")
            with self.assertRaises(RuntimeError):
                self.card_install(card, "1.4.0.7")
            self.assertEqual(sorted(os.listdir(card)), ["autorun.sh"])

    def test_top_autorun_runs_the_autorun_folder_script_as_asked(self):
        from device import TOP_AUTORUN
        with tempfile.TemporaryDirectory() as temp:
            card = Path(temp)
            (card / "autorun").mkdir()
            (card / "autorun/autorun.sh").write_text('echo "$0 ${SAMPLE_BASE_ASKED:-}"\n')
            (card / "autorun.sh").write_text(TOP_AUTORUN.read_text())
            # As the 1.4 stock app runs it: /bin/sh with the script's full path, no environment.
            result = subprocess.run(["/bin/sh", str(card / "autorun.sh")], env={}, capture_output=True, text=True)
            self.assertEqual(result.stdout, str(card) + "/autorun/autorun.sh 1\n")

    def test_uninstall_takes_out_only_our_lines(self):
        from device import chain_autorun, without_marker, SD_MARKER
        plain = "#!/bin/sh\necho ssh\n"
        self.assertEqual(without_marker(chain_autorun(plain), SD_MARKER, "sample-base/sd-boot.sh"), plain)
