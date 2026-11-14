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
            compile_fixture(binary, "tests/clock_io.cpp", "src/drivers/clock_io.cpp",
                            extra=("-Wl,--wrap=open", "-Wl,--wrap=close", "-Wl,--wrap=fcntl",
                                   "-Wl,--wrap=ioctl", "-Wl,--wrap=read"))
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_power_button_evdev_press_release(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "power"
            compile_fixture(binary, "tests/power_button.cpp", "src/drivers/power_button.cpp", "src/drivers/platform.cpp")
            subprocess.run([str(binary), str(Path(temp) / "fixture")], check=True, timeout=5)

    def test_mixer_ranges_exclusive_inputs_and_restore(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "mixer"
            symbols = ("snd_ctl_open", "snd_ctl_close", "snd_ctl_elem_info", "snd_ctl_elem_read", "snd_ctl_elem_write",
                       "snd_ctl_elem_info_get_type", "snd_ctl_elem_info_get_count", "snd_ctl_elem_info_is_readable",
                       "snd_ctl_elem_info_is_writable", "snd_ctl_elem_info_get_min", "snd_ctl_elem_info_get_max")
            compile_fixture(binary, "tests/mixer.cpp", "src/drivers/mixer.cpp", "src/drivers/codec_power.cpp",
                            "src/drivers/platform.cpp", extra=("-lasound", *("-Wl,--wrap=" + name for name in symbols)))
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_jack_detection_events_initial_states_and_card_loss(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "jacks"
            symbols = ("snd_ctl_open", "snd_ctl_close", "snd_ctl_subscribe_events", "snd_ctl_read", "snd_ctl_elem_read")
            compile_fixture(binary, "tests/jacks.cpp", "src/drivers/jacks.cpp", "src/drivers/platform.cpp",
                            extra=("-lasound", *("-Wl,--wrap=" + name for name in symbols)))
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_midi_hotplug_partial_output_and_disconnect_panic(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "midi-ports"
            symbols = ("snd_card_next", "snd_ctl_open", "snd_ctl_close", "snd_ctl_card_info", "snd_ctl_card_info_get_id",
                       "snd_ctl_card_info_get_name", "snd_ctl_card_info_get_driver", "snd_ctl_rawmidi_next_device",
                       "snd_ctl_rawmidi_info", "snd_rawmidi_info_get_subdevices_count", "snd_rawmidi_info_get_subdevice_name",
                       "snd_rawmidi_open", "snd_rawmidi_close", "snd_rawmidi_drop", "snd_rawmidi_read", "snd_rawmidi_write",
                       "snd_rawmidi_params_current")
            compile_fixture(binary, "tests/hardware_midi.cpp", "src/drivers/hardware_midi.cpp", "src/drivers/platform.cpp",
                            extra=("-lasound", *("-Wl,--wrap=" + name for name in symbols)))
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_partial_audio_io_and_xrun_recovery(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "audio"
            compile_fixture(binary, "tests/audio.cpp", "src/drivers/audio.cpp", "src/drivers/codec_power.cpp", "src/drivers/platform.cpp",
                            extra=("-lasound", "-Wl,--wrap=snd_pcm_readi", "-Wl,--wrap=snd_pcm_writei",
                                   "-Wl,--wrap=snd_pcm_wait", "-Wl,--wrap=snd_pcm_prepare", "-Wl,--wrap=snd_pcm_start"))
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_usb_gadget_both_ways_discovery_rate_changes_drift_and_conversion(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "usb-audio"
            symbols = ("snd_card_next", "snd_ctl_open", "snd_ctl_close", "snd_ctl_card_info", "snd_ctl_card_info_get_driver",
                       "snd_ctl_card_info_get_id", "snd_ctl_elem_read", "snd_pcm_open", "snd_pcm_close", "snd_pcm_wait",
                       "snd_pcm_readi", "snd_pcm_writei", "snd_pcm_hw_params_get_channels_min")
            # Optimised: the drift test converts four minutes of host audio.
            compile_fixture(binary, "tests/usb_audio.cpp", "src/drivers/usb_audio.cpp", "src/drivers/platform.cpp",
                            extra=("-O2", "-lasound", *("-Wl,--wrap=" + name for name in symbols)))
            subprocess.run([str(binary)], check=True, timeout=60)

    def test_usb_gadget_widened_for_stems(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "usb-gadget"
            compile_fixture(binary, "tests/usb_gadget.cpp", "src/drivers/usb_gadget.cpp")
            subprocess.run([str(binary), temp], check=True, timeout=10)

    def test_screen_mirror_picture_scaled_centred_and_yuyv(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "mirror-frame"
            compile_fixture(binary, "tests/mirror_frame.cpp", "src/drivers/mirror_frame.cpp")
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_card_share_found_refused_and_recovered(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "card-share"
            compile_fixture(binary, "tests/card_share.cpp", "src/drivers/card_share.cpp", "src/drivers/usb_gadget.cpp",
                            "src/drivers/platform.cpp", "src/drivers/system_services.cpp", extra=("-lsystemd",))
            subprocess.run([str(binary), temp], check=True, timeout=20,
                           env={**os.environ, "DBUS_SYSTEM_BUS_ADDRESS": "unix:path=/nonexistent-sample-base-bus"})

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
                # USB output is on by default and waits for a gadget card that this host lacks.
                usb = json.loads(command("usb-output")[3:])
                self.assertEqual((usb["requested"], usb["opens"], usb["rate"]), ("auto", 0, 0))
                self.assertIn(usb["state"], ("missing", "waiting"))
                self.assertEqual(command("usb-output off"), "OK usb-output off")
                self.assertIn("usb_output=stopped", command("status"))
                self.assertEqual(command("usb-output on"), "OK usb-output on")
                self.assertNotIn("usb_output=stopped", command("status"))
                self.assertTrue(command("usb-output maybe").startswith("ERR"))
                self.assertTrue(command("prompt STEP 1/2|K1|Turn left||Pad 1 next").startswith("OK"))
                self.assertTrue(command("prompt " + "x" * 401).startswith("ERR"))
                self.assertTrue(command("prompt").startswith("OK"))
                self.assertTrue(command("surface-sync").startswith("OK"))
                for good in ("led pad 1 127 0 0", "led pad 16 0 0 127", "led fader 101 101 101", "led bank A on",
                             "led bank H off", "led button Stop bright", "led button sample-select red-dim",
                             "led button 16levels red-bright", "led meter L 31", "led meter R 3", "led off"):
                    self.assertTrue(command(good).startswith("OK"), good)
                for bad in ("led pad 0 1 1 1", "led pad 17 1 1 1", "led pad 1 128 0 0", "led fader 1 2",
                            "led bank I on", "led bank A dim", "led button Shift dim", "led button Stop red-dim",
                            "led button Nope dim", "led meter L 32", "led meter C 1", "led meter R", "led off now", "led"):
                    self.assertTrue(command(bad).startswith("ERR"), bad)
                power = json.loads(command("power")[3:])
                self.assertEqual((power["key"], power["presses"]), ("KEY_POWER", 0))
                jacks = json.loads(command("jacks")[3:])  # No I2S card here: unknown, never reported.
                self.assertEqual((jacks["available"], jacks["jacks"]["headphones"]["plugged"], jacks["jacks"]["line_out"]["changes"]),
                                 (False, -1, 0))
                self.assertIn("headphones=-1 line_out=-1 push_exit=", command("status"))
                self.assertTrue(command("jacks now").startswith("ERR"))

                def recorder(until=lambda state: True):
                    for _ in range(300):
                        state = json.loads(command("recorder")[3:])
                        if until(state):
                            return state
                        time.sleep(0.01)
                    self.fail("recorder never reached the expected state: " + json.dumps(state))
                self.assertIn("source=off channels=stereo recorder=idle", command("status"))
                self.assertTrue(command("transport record").startswith("OK"))
                self.assertEqual(recorder(lambda state: state["refused"] == 1)["frames"], 0)
                for bad in ("source radio", "channels middle", "transport rewind", "page nope", "recorder now", "input usb"):
                    self.assertTrue(command(bad).startswith("ERR"), bad)
                self.assertTrue(command("input line").startswith("ERR"))  # No I2S mixer here.
                self.assertTrue(command("source line").startswith("OK source line"))
                self.assertTrue(command("channels left").startswith("OK"))
                self.assertTrue(command("page recorder").startswith("OK"))
                self.assertTrue(command("transport record").startswith("OK"))
                recorder(lambda state: state["frames"] > 0)
                self.assertTrue(command("transport stop").startswith("OK"))
                take = recorder(lambda state: state["state"] == "idle")
                self.assertEqual((take["take_source"], take["take_channels"], take["takes"]), ("line", "left", 1))
                self.assertEqual((take["capacity"], take["rate"]), (60 * 44100, 44100))
                self.assertTrue(command("transport play").startswith("OK"))
                recorder(lambda state: state["state"] == "idle")
                self.assertTrue(command("source usb").startswith("OK"))
                self.assertEqual(recorder(lambda state: state["usb"]["state"] == "missing")["source"], "usb")
                self.assertTrue(command("source off").startswith("OK"))
                self.assertIn("source=off channels=left", command("status"))
                self.assertIn("usb=stopped", command("status"))
                self.assertTrue(command("page synth").startswith("OK"))

                def status_until(text):
                    for _ in range(300):
                        reply = command("status")
                        if text in reply:
                            return reply
                        time.sleep(0.01)
                    self.fail("status never showed " + text + ": " + reply)
                self.assertIn("page=synth light_scene=0", command("status"))
                self.assertTrue(command("page lights").startswith("OK"))
                status_until("page=lights light_scene=1")
                self.assertEqual(command("lights 4"), "OK lights scene 4 Rainbow")
                status_until("light_scene=4")
                self.assertTrue(command("lights next").startswith("OK lights scene 5"))
                status_until("light_scene=5")
                self.assertTrue(command("lights previous").startswith("OK lights scene 4"))
                for bad in ("lights", "lights 0", "lights 11", "lights first"):
                    self.assertTrue(command(bad).startswith("ERR"), bad)
                self.assertTrue(command("page synth").startswith("OK"))
                status_until("page=synth light_scene=0")
                self.assertIn("pressure=0 peak_pressure=0", command("status"))
                self.assertEqual(command("page pads"), "OK page pads")
                status_until("page=pads")
                self.assertTrue(command("page synth").startswith("OK"))
                status_until("page=synth")

                self.assertIn("seq=stopped seq_step=0 tempo=110.0", command("status"))
                self.assertEqual(command("page seq"), "OK page seq")
                status_until("page=seq")
                self.assertEqual(command("seq tempo 92.5"), "OK seq tempo 92.5")
                self.assertEqual(command("seq step b 5 100"), "OK seq step B 5 100")
                self.assertEqual(command("seq mute H on"), "OK seq mute H on")
                for bad in ("seq tempo 39", "seq tempo 241", "seq tempo 92.55", "seq tempo -90", "seq step I 1 1",
                            "seq step A 0 1", "seq step A 17 1", "seq step A 1 128", "seq mute A maybe",
                            "seq rewind", "seq play now", "seq step A 1 1 1"):
                    self.assertTrue(command(bad).startswith("ERR"), bad)
                pattern = json.loads(command("seq")[3:])
                self.assertEqual((pattern["state"], pattern["step"], pattern["tempo"], pattern["track"], pattern["pads"], pattern["armed"]),
                                 ("stopped", 0, 92.5, "A", "steps", False))
                self.assertEqual([track["drum"] for track in pattern["tracks"]],
                                 ["Kick", "Snare", "Clap", "Closed hat", "Open hat", "Low tom", "High tom", "Bell"])
                self.assertEqual(pattern["tracks"][0]["steps"], [112, 0, 0, 0] * 4)
                self.assertEqual(pattern["tracks"][1]["steps"][4], 100)
                self.assertTrue(pattern["tracks"][7]["muted"])
                self.assertTrue(command("seq play").startswith("OK"))
                status_until("seq=playing")

                def sequencer(until):
                    for _ in range(300):
                        state = json.loads(command("seq")[3:])
                        if until(state):
                            return state
                        time.sleep(0.01)
                    self.fail("sequencer never reached the expected state: " + json.dumps(state))
                # The audio thread plays the bar against the null PCM, and it
                # keeps playing on another page.
                sequencer(lambda state: state["tracks"][1]["hits"] > 0 and state["tracks"][0]["hits"] > 1)
                self.assertTrue(command("page synth").startswith("OK"))
                status_until("page=synth")
                self.assertIn("seq=playing", command("status"))
                self.assertTrue(command("seq stop").startswith("OK"))
                status_until("seq=stopped seq_step=0")
                self.assertEqual(command("seq reset"), "OK seq reset")
                reset = json.loads(command("seq")[3:])
                self.assertEqual((reset["tempo"], reset["tracks"][1]["steps"][4], reset["tracks"][7]["muted"]), (110.0, 0, False))

                # The system page reads the hardware on the service thread,
                # only while it is open.
                closed = json.loads(command("system")[3:])
                self.assertEqual((closed["reads"], closed["panel"], closed["cpu"]), (0, "health", []))
                self.assertEqual(command("page system"), "OK page system")
                status_until("page=system")

                def system(until):
                    for _ in range(300):
                        state = json.loads(command("system")[3:])
                        if until(state):
                            return state
                        time.sleep(0.01)
                    self.fail("system page never read: " + json.dumps(state))
                # Two /proc/stat samples half a second apart give the loads: total, then each core.
                state = system(lambda state: len(state["cpu"]) > 1)
                self.assertTrue(all(0 <= load <= 1 for load in state["cpu"]), state["cpu"])
                self.assertGreater(state["memory_total_kb"], 0)
                self.assertLessEqual(state["memory_available_kb"], state["memory_total_kb"])
                self.assertEqual(state["jacks"], {"headphones": -1, "line_out": -1})  # No I2S mixer here.
                self.assertEqual(state["usb_in"]["state"], "stopped")
                self.assertIn(state["usb_out"]["state"], ("missing", "waiting"))
                self.assertEqual(state["power_key"]["presses"], 0)
                self.assertEqual(state["midi"]["send_drops"], 0)
                self.assertIsInstance(state["storage"], list)
                self.assertEqual(command("system next"), "OK system audio")
                self.assertEqual(command("system previous"), "OK system health")
                self.assertEqual(command("system storage"), "OK system storage")
                self.assertEqual(json.loads(command("system")[3:])["panel"], "storage")
                for bad in ("system disk", "system next now", "system 2"):
                    self.assertTrue(command(bad).startswith("ERR"), bad)
                self.assertTrue(command("page synth").startswith("OK"))
                status_until("page=synth")
                reads = json.loads(command("system")[3:])["reads"]
                time.sleep(0.7)
                self.assertEqual(json.loads(command("system")[3:])["reads"], reads)
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
    def test_starter_app_runs_on_the_runtime_alone(self):
        env = {**os.environ, "DBUS_SYSTEM_BUS_ADDRESS": "unix:path=/nonexistent-sample-base-bus"}
        unknown = subprocess.run([str(ROOT / "build/sample-base"), "--app", "nope", "--headless", "--no-surface"],
                                 capture_output=True, text=True, timeout=10, env=env)
        self.assertEqual(unknown.returncode, 1)
        self.assertIn("Unknown app: nope", unknown.stderr)
        with tempfile.TemporaryDirectory() as temp:
            runtime = Path(temp) / "runtime"
            app = subprocess.Popen([str(ROOT / "build/sample-base"), "--app", "starter", "--headless", "--no-surface",
                                    "--audio-output", "null", "--audio-input", "off", "--usb-output", "off", "--runtime-dir", str(runtime),
                                    "--seconds", "10"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
            client = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            try:
                for _ in range(200):
                    if (runtime / "control.sock").exists() or app.poll() is not None:
                        break
                    time.sleep(0.01)
                self.assertIsNone(app.poll(), "starter app failed to start")
                client.bind(str(runtime / "test.sock")); client.settimeout(4)
                def command(text):
                    client.sendto(text.encode(), str(runtime / "control.sock"))
                    return client.recv(32768).decode()
                status = command("status")
                # The runtime's fields only: the starter adds none.
                self.assertTrue(status.startswith("OK frames=") and status.endswith("push_exit=1 usb_output=stopped card_share=home "
                                                                     "audio_rate=44100 audio_periods=3 audio_latency_frames=384 audio_reopens=0 "
                                                                     "mirror=off mirror_size=0x0 mirror_fps=0 mirror_frames=0 mirror_late=0 mirror_bad=0 mirror_held=0"), status) # headless: no Screen Mirror
                self.assertNotIn("page=", status)
                self.assertEqual(command("audition"), "OK audition pad 1")
                for _ in range(300):
                    if "synthetic_events=2" in command("status"):
                        break
                    time.sleep(0.01)
                status = command("status")
                self.assertIn("synthetic_events=2 held=0000 last_pad=1 velocity=100", status)
                self.assertTrue(command("page pads").startswith("ERR use status"))
                self.assertNotIn("app:", command("page pads"))
                self.assertTrue(command("stock").startswith("OK"))
                stdout, stderr = app.communicate(timeout=5)
                self.assertEqual(app.returncode, 0, stderr)
                self.assertIn("READY sample-base", stdout)
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
