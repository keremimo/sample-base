import os
import shlex
import subprocess
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from device import Device, service_unit, stale_releases

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
        device = Device()
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

    def test_uninstall_takes_out_only_our_lines(self):
        from device import chain_autorun, without_marker, SD_MARKER
        plain = "#!/bin/sh\necho ssh\n"
        self.assertEqual(without_marker(chain_autorun(plain), SD_MARKER, "sample-base/sd-boot.sh"), plain)
