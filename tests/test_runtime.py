# SPDX-License-Identifier: AGPL-3.0-only
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from test_platform import compile_fixture

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == "linux", "requires Linux ALSA libraries")
class RuntimeTests(unittest.TestCase):
    def test_sync_lines_are_opt_in_and_preserve_kernel_ownership(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "clock"
            compile_fixture(binary, "tests/clock_io.cpp", "src/clock_io.cpp",
                            extra=("-Wl,--wrap=open", "-Wl,--wrap=close", "-Wl,--wrap=fcntl",
                                   "-Wl,--wrap=ioctl", "-Wl,--wrap=read"))
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_power_button_evdev_press_release(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "power"
            compile_fixture(binary, "tests/power_button.cpp", "src/power_button.cpp", "src/platform.cpp")
            subprocess.run([str(binary), str(Path(temp) / "fixture")], check=True, timeout=5)

    def test_mixer_ranges_exclusive_inputs_and_restore(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "mixer"
            symbols = ("snd_ctl_open", "snd_ctl_close", "snd_ctl_elem_info", "snd_ctl_elem_read", "snd_ctl_elem_write",
                       "snd_ctl_elem_info_get_type", "snd_ctl_elem_info_get_count", "snd_ctl_elem_info_is_readable",
                       "snd_ctl_elem_info_is_writable", "snd_ctl_elem_info_get_min", "snd_ctl_elem_info_get_max")
            compile_fixture(binary, "tests/mixer.cpp", "src/mixer.cpp", "src/platform.cpp",
                            extra=("-lasound", *("-Wl,--wrap=" + name for name in symbols)))
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_midi_hotplug_partial_output_and_disconnect_panic(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "midi-ports"
            symbols = ("snd_card_next", "snd_ctl_open", "snd_ctl_close", "snd_ctl_card_info", "snd_ctl_card_info_get_id",
                       "snd_ctl_card_info_get_name", "snd_ctl_card_info_get_driver", "snd_ctl_rawmidi_next_device",
                       "snd_ctl_rawmidi_info", "snd_rawmidi_info_get_subdevices_count", "snd_rawmidi_info_get_subdevice_name",
                       "snd_rawmidi_open", "snd_rawmidi_close", "snd_rawmidi_drop", "snd_rawmidi_read", "snd_rawmidi_write")
            compile_fixture(binary, "tests/hardware_midi.cpp", "src/hardware_midi.cpp", "src/platform.cpp",
                            extra=("-lasound", *("-Wl,--wrap=" + name for name in symbols)))
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_partial_audio_io_and_xrun_recovery(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "audio"
            compile_fixture(binary, "tests/audio.cpp", "src/audio.cpp", "src/platform.cpp",
                            extra=("-lasound", "-Wl,--wrap=snd_pcm_readi", "-Wl,--wrap=snd_pcm_writei",
                                   "-Wl,--wrap=snd_pcm_wait", "-Wl,--wrap=snd_pcm_prepare", "-Wl,--wrap=snd_pcm_start"))
            subprocess.run([str(binary)], check=True, timeout=10)

    @unittest.skipUnless((ROOT / "build/sample-base").is_file(), "build application first")
    def test_headless_control_commands_and_clean_shutdown(self):
        with tempfile.TemporaryDirectory() as temp:
            runtime = Path(temp) / "runtime"
            app = subprocess.Popen([str(ROOT / "build/sample-base"), "--headless", "--no-surface", "--audio-output", "null",
                                    "--audio-input", "off", "--runtime-dir", str(runtime), "--seconds", "10"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                   env={**os.environ, "DBUS_SYSTEM_BUS_ADDRESS": "unix:path=/nonexistent-sample-base-bus"})
            client = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            try:
                for _ in range(200):
                    if (runtime / "control.sock").exists() or app.poll() is not None:
                        break
                    time.sleep(0.01)
                self.assertIsNone(app.poll(), "headless app failed to start")
                client.bind(str(runtime / "test.sock")); client.settimeout(4)
                def command(text):
                    client.sendto(text.encode(), str(runtime / "control.sock"))
                    return client.recv(32768).decode()
                self.assertIn("capture_active=0", command("status"))
                self.assertTrue(command("audition").startswith("OK"))
                self.assertTrue(command("gain 50").startswith("OK"))
                self.assertTrue(command("monitor 0").startswith("OK"))
                self.assertTrue(command("gain 101").startswith("ERR"))
                self.assertTrue(command("monitor -1").startswith("ERR"))
                self.assertTrue(command("midi-send missing 90 3c").startswith("ERR"))
                self.assertTrue(command("storage-mount /tmp/invalid").startswith("ERR"))
                self.assertTrue(command("unknown").startswith("ERR"))
                hardware = json.loads(command("hardware")[3:])
                self.assertIn("power", hardware["platform"])
                self.assertFalse(json.loads(command("clock")[3:])["input_enabled"])
                self.assertTrue(command("clock-in off").startswith("OK"))
                self.assertIn("ports", hardware["midi"])
                self.assertFalse(json.loads(command("storage")[3:])["state"]["available"])
                self.assertEqual(len(json.loads(command("surface")[3:])["state"]["controls"]), 128)
                log = json.loads(command("surface-log 0")[3:])
                self.assertEqual((log["newest"], log["next"], log["packets"]), (0, 0, []))
                self.assertTrue(command("surface-log -1").startswith("ERR"))
                self.assertIn("push_exit=1", command("status"))
                self.assertTrue(command("push-exit off").startswith("OK"))
                self.assertIn("push_exit=0", command("status"))
                self.assertTrue(command("push-exit maybe").startswith("ERR"))
                self.assertTrue(command("push-exit on").startswith("OK"))
                self.assertTrue(command("prompt STEP 1/2|K1|Turn left||Pad 1 next").startswith("OK"))
                self.assertTrue(command("prompt " + "x" * 401).startswith("ERR"))
                self.assertTrue(command("prompt").startswith("OK"))
                self.assertTrue(command("surface-sync").startswith("OK"))
                for good in ("led pad 1 127 0 0", "led pad 16 0 0 127", "led fader 101 101 101", "led bank A on",
                             "led bank H off", "led button Stop bright", "led button sample-select red-dim",
                             "led button 16levels red-bright", "led off"):
                    self.assertTrue(command(good).startswith("OK"), good)
                for bad in ("led pad 0 1 1 1", "led pad 17 1 1 1", "led pad 1 128 0 0", "led fader 1 2",
                            "led bank I on", "led bank A dim", "led button Shift dim", "led button Stop red-dim",
                            "led button Nope dim", "led off now", "led"):
                    self.assertTrue(command(bad).startswith("ERR"), bad)
                power = json.loads(command("power")[3:])
                self.assertEqual((power["key"], power["presses"]), ("KEY_POWER", 0))
                stream = subprocess.run([str(ROOT / "build/sample-base"), "--runtime-dir", str(runtime), "--ctl-stream"],
                                        input="status\nunknown\nsurface-log 0\n", capture_output=True, text=True, timeout=15)
                replies = stream.stdout.splitlines()
                self.assertEqual(stream.returncode, 0, stream.stderr)
                self.assertEqual(len(replies), 3)
                self.assertTrue(replies[0].startswith("OK frames=") and replies[1].startswith("ERR"))
                self.assertEqual(json.loads(replies[2][3:])["packets"], [])
                self.assertTrue(command("stock").startswith("OK"))
                stdout, stderr = app.communicate(timeout=5)
                self.assertEqual(app.returncode, 0, stderr)
                self.assertIn("READY sample-base", stdout)
                self.assertFalse((runtime / "control.sock").exists())
            finally:
                client.close()
                if app.poll() is None:
                    app.kill()
                app.communicate(timeout=5)

    @unittest.skipUnless((ROOT / "build/sample-base").is_file(), "build application first")
    def test_inventory_without_opening_pcm_or_surface(self):
        result = subprocess.run([str(ROOT / "build/sample-base"), "--hardware"], capture_output=True, text=True, timeout=5,
                                env={**os.environ, "DBUS_SYSTEM_BUS_ADDRESS": "unix:path=/nonexistent-sample-base-bus"})
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertIn("platform", data)
        self.assertIn("audio", data)
        self.assertIsInstance(data["midi"], list)
