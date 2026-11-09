# SPDX-License-Identifier: AGPL-3.0-only
import itertools
import json
import unittest
from tools.capture_controls import STEPS, Capture, pad_notes, summarize

NOTES = pad_notes()
NEXT, REDO, BACK = ([0x99, NOTES[n], 100] for n in range(3))


class FakeControl:
    """Reveals one scripted surface packet per surface-log poll."""

    def __init__(self, packets):
        self.packets, self.visible, self.polls, self.commands = packets, 0, 0, []
        self.volume = 10
        self.power = {"available": True, "device": "/dev/input/event0", "key": "KEY_POWER", "pressed": False,
                      "presses": 4, "timestamp_ns": 0}

    def __call__(self, command):
        self.commands.append(command)
        if command.startswith("surface-log"):
            since = int(command.split()[1])
            self.polls += 1
            if self.polls > 1:
                self.visible = min(len(self.packets), self.visible + 1)
            if self.visible and self.packets[self.visible - 1] == "volume":
                self.volume = 4
            entries = [[n + 1, 1000 * (n + 1), 3, packet] for n, packet in enumerate(self.packets[:self.visible])
                       if n + 1 > since and packet not in ("volume", "power")]
            return json.dumps({"oldest": 1, "newest": self.visible, "diagnostic_overflows": 0,
                               "packets": entries, "next": max(since, self.visible)})
        if command == "power":
            if self.visible and self.packets[self.visible - 1] == "power":
                self.power = dict(self.power, presses=5, timestamp_ns=77)  # pressed and released between polls
            return json.dumps(self.power)
        if command == "mixer":
            return json.dumps({"controls": [{"id": 3, "name": "Master", "values": [self.volume]}]})
        return "prompt shown" if command.startswith("prompt") else ""


class CaptureTests(unittest.TestCase):
    def test_steps_cover_every_named_control_once(self):
        controls = [step["control"] for step in STEPS]
        self.assertEqual(len(set(controls)), 31)
        self.assertEqual(sum(step["gesture"] == "press" for step in STEPS), 25)
        self.assertEqual([step["control"] for step in STEPS if step.get("source")], ["power"])
        self.assertFalse(any("|" in step["label"] + step["detail"] for step in STEPS))
        self.assertEqual(len(NOTES), 16)

    def test_pads_navigate_and_are_never_recorded(self):
        steps = [
            {"control": "main_volume", "label": "Main volume", "gesture": "decrease", "detail": "Turn",
             "mixer": True, "optional": True},
            {"control": "k1", "label": "K1", "gesture": "decrease", "detail": "Turn", "mixer": False},
            {"control": "play", "label": "Play", "gesture": "press", "detail": "Press", "mixer": False},
        ]
        script = ["volume", NEXT,                              # analog-only control: empty is accepted
                  [0xb0, 16, 0x7f], [0x99, NOTES[4], 90], [0xa9, NOTES[4], 3], REDO,
                  [0xb0, 16, 0x7d], [0xb0, 16, 0x7e], NEXT,
                  NEXT,                                        # empty button step needs a second tap
                  BACK, [0xb0, 16, 0x01], NEXT,                # back discards and replaces K1
                  [0x90, 0x30, 0x7f], [0x80, 0x30, 0], NEXT]
        control, events = FakeControl(script), []
        ticks = itertools.count(0, 0.1)
        capture = Capture(control, steps, NOTES, events.append, debounce=0, finish_seconds=0,
                          clock=lambda: next(ticks), sleep=lambda _: None)
        self.assertTrue(capture.run())
        self.assertEqual([event["event"] for event in events],
                         ["ready", "step", "step", "back", "step", "step", "complete"])
        volume, k1, play = (capture.recorded[n] for n in range(3))
        self.assertEqual(volume["packets"], [])
        self.assertEqual(volume["mixer_changes"], {"3 Master": [[10], [4]]})
        self.assertEqual([p["bytes"] for p in k1["packets"]], [[0xb0, 16, 0x01]])
        self.assertEqual([(i["kind"], i["channel"], i["number"]) for i in play["identities"]],
                         [("note_on", 1, 0x30), ("note_off", 1, 0x30)])
        self.assertTrue(any("Nothing captured" in c for c in control.commands))
        self.assertEqual(control.commands[0], "push-exit off")
        self.assertEqual(control.commands[-2:], ["prompt", "push-exit on"])

    def test_power_key_is_read_from_input_state(self):
        steps = [dict(step) for step in STEPS if step["control"] == "power"]
        control = FakeControl(["power", NEXT])
        ticks = itertools.count(0, 0.1)
        capture = Capture(control, steps, NOTES, debounce=0, finish_seconds=0, clock=lambda: next(ticks), sleep=lambda _: None)
        self.assertTrue(capture.run())
        self.assertEqual([(p["input"], p["pressed"]) for p in capture.recorded[0]["packets"]],
                         [("KEY_POWER", True), ("KEY_POWER", False)])
        self.assertEqual(capture.recorded[0]["identities"], [
            {"kind": "input", "channel": None, "number": "KEY_POWER", "count": 2, "first": 1, "last": 0,
             "minimum": 0, "maximum": 1, "distinct": [0, 1]}])

    def test_failure_restores_wheel_push_and_clears_prompt(self):
        commands = []

        def failing(command):
            commands.append(command)
            if command.startswith("surface-log"):
                raise RuntimeError("surface-log: ERR")
            return ""
        with self.assertRaises(RuntimeError):
            Capture(failing, STEPS, NOTES, sleep=lambda _: None).run()
        self.assertEqual(commands[-2:], ["prompt", "push-exit on"])

    def test_summary_groups_by_channel_kind_and_number(self):
        packets = [{"bytes": data} for data in ([0xb1, 7, 65], [0xb1, 7, 63], [0xb1, 7, 65], [0xe0, 0, 64], [0xf0, 1, 0xf7])]
        self.assertEqual(summarize(packets), [
            {"kind": "cc", "channel": 2, "number": 7, "count": 3, "first": 65, "last": 65,
             "minimum": 63, "maximum": 65, "distinct": [63, 65]},
            {"kind": "pitch_bend", "channel": 1, "number": None, "count": 1, "first": 8192, "last": 8192,
             "minimum": 8192, "maximum": 8192, "distinct": [8192]},
            {"kind": "sysex", "channel": None, "number": "F0 01 F7", "count": 1},
        ])


if __name__ == "__main__":
    unittest.main()
