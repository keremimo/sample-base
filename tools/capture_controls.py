#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Guided capture of the MPC Sample's non-pad controls.

A running sample-base session shows each step on the device screen. Use the
named control, then tap pad 1 for the next step, pad 2 to redo the current
step or pad 3 to go back one step. Every surface packet between those taps is
read through the lossless `surface-log` command over one SSH session. Raw
captures are device data and are written only to ignored `.local/`.

Wheel Push normally returns to the stock app, so the capture disables that
shortcut while it runs. Remote stop, the session deadline and crash recovery
still return to stock; the shortcut is restored when the capture ends.

The power button is a Linux input key, not surface MIDI. Its step reads the
app's `power` command instead; press it briefly, because a long hold can
trigger the power-management chip's hardware power-off.

Re-run selected controls with --only, for example `--only power,k1`.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from device import ACTIVE, Device  # noqa: E402

BUTTON = "Press and release once"


def _turn(control, label, mixer=False, down="Turn left a few steps", up="Turn right a few steps"):
    return [{"control": control, "label": label, "gesture": "decrease", "detail": down, "mixer": mixer},
            {"control": control, "label": label, "gesture": "increase", "detail": up, "mixer": mixer}]


def _button(control, label, detail=BUTTON):
    return {"control": control, "label": label, "gesture": "press", "detail": detail, "mixer": False}


# Physical order: knobs and fader, left column, the row above the screen,
# then the right-hand buttons. The main volume may be analog-only, so an
# empty capture is accepted for it and ALSA mixer changes are recorded.
STEPS = [
    *_turn("main_volume", "Main volume", mixer=True, down="Turn down a little", up="Turn back up"),
    *_turn("k1", "K1"), *_turn("k2", "K2"), *_turn("k3", "K3"),
    *_turn("wheel", "Wheel", down="Turn left 3 clicks", up="Turn right 3 clicks"),
    _button("wheel_push", "Wheel push", "Press the wheel and release"),
    *_turn("fader", "Fader", down="Slide fully down", up="Slide fully up"),
    _button("sample", "Sample"), _button("seq", "SEQ"), _button("pad_fx", "Pad FX"),
    _button("knob_fx", "Knob FX"), _button("shift", "Shift"), _button("pad_bank", "Pad Bank"),
    _button("erase", "Erase"), _button("note_repeat", "Note Repeat"),
    _button("top_left", "Top left", "Leftmost button above screen"),
    _button("top_middle", "Top middle", "Middle button above screen"),
    _button("top_right", "Top right", "Rightmost button above screen"),
    _button("chop", "Chop"), _button("mute", "Mute"), _button("loop", "Loop"),
    _button("sixteen_levels", "16 Levels"), _button("sample_select", "Sample Select"),
    _button("tap_tempo", "Tap Tempo"), _button("minus", "Minus  ( - )"), _button("plus", "Plus  ( + )"),
    _button("sample_record", "Sample Record"), _button("seq_record", "Seq Record"),
    _button("stop", "Stop"), _button("play", "Play"),
    dict(_button("power", "Power", "Short press, under 1 second"), source="power"),
]
for _step in STEPS:
    _step["optional"] = _step["control"] == "main_volume"

KINDS = {0x80: "note_off", 0x90: "note_on", 0xa0: "poly_pressure", 0xb0: "cc",
         0xc0: "program", 0xd0: "channel_pressure", 0xe0: "pitch_bend"}
FOOTER = "Pad 1 next     Pad 2 redo     Pad 3 back"


def pad_notes():
    source = (ROOT / "src/pads.h").read_text()
    match = re.search(r"surface_pad_notes\s*\{([^}]*)\}", source)
    notes = [int(value) for value in match.group(1).split(",")] if match else []
    if len(notes) != 16 or len(set(notes)) != 16:
        raise RuntimeError("src/pads.h does not contain 16 distinct surface pad notes")
    return notes


def identity(data):
    """Group key for one packet: kind, 1-based channel and note/CC number."""
    if isinstance(data, dict):
        return ("input", None, data["input"])
    if not data:
        return ("empty", None, None)
    status = data[0]
    if status >= 0xf0:
        return ("sysex" if status == 0xf0 else "system", None, " ".join("%02X" % b for b in data[:6]))
    kind = KINDS[status & 0xf0]
    number = data[1] if kind in ("note_off", "note_on", "poly_pressure", "cc") and len(data) > 1 else None
    return (kind, (status & 0x0f) + 1, number)


def value(data):
    if isinstance(data, dict):
        return int(data["pressed"])
    status = data[0] if data else 0xf0
    if status >= 0xf0:
        return None
    if status & 0xf0 == 0xe0 and len(data) == 3:
        return data[1] | data[2] << 7
    if status & 0xf0 in (0xc0, 0xd0):
        return data[1] if len(data) > 1 else None
    return data[2] if len(data) > 2 else None


def summarize(packets):
    """Distinct message identities in first-seen order, with value statistics."""
    groups = {}
    for packet in packets:
        data = packet if "input" in packet else packet["bytes"]
        group = groups.setdefault(identity(data), {"count": 0, "values": []})
        group["count"] += 1
        if value(data) is not None:
            group["values"].append(value(data))
    result = []
    for (kind, channel, number), group in groups.items():
        values = group["values"]
        entry = {"kind": kind, "channel": channel, "number": number, "count": group["count"]}
        if values:
            entry.update(first=values[0], last=values[-1], minimum=min(values), maximum=max(values),
                         distinct=sorted(set(values))[:16])
        result.append(entry)
    return result


def describe(entry):
    where = (" ch%d" % entry["channel"] if entry["channel"] else "") + \
            (" #%s" % entry["number"] if entry["number"] is not None else "")
    values = " values %d..%d (first %d, last %d)" % (entry["minimum"], entry["maximum"], entry["first"], entry["last"]) \
        if "first" in entry else ""
    return "%s%s x%d%s" % (entry["kind"], where, entry["count"], values)


def mixer_values(ctl):
    try:
        data = json.loads(ctl("mixer"))
    except (RuntimeError, ValueError) as error:
        return {"error": str(error)}
    return {"%s %s" % (c["id"], c["name"]): c.get("values") for c in data.get("controls", [])}


def mixer_changes(before, after):
    if before is None or "error" in before or "error" in after:
        return {"before": before, "after": after}
    return {name: [before.get(name), values] for name, values in after.items() if before.get(name) != values}


class Capture:
    """Pad-driven step machine. `ctl` sends one command and returns the text
    after `OK `, raising RuntimeError for any other reply."""

    def __init__(self, ctl, steps, notes, report=lambda event: None, seconds=1500, debounce=0.35,
                 finish_seconds=3, clock=time.monotonic, sleep=time.sleep):
        self.ctl, self.steps, self.report = ctl, steps, report
        self.pads = set(notes)
        self.navigation = {notes[0]: "next", notes[1]: "redo", notes[2]: "back"}
        self.seconds, self.debounce, self.finish_seconds = seconds, debounce, finish_seconds
        self.clock, self.sleep = clock, sleep
        self.recorded = {}
        self.index, self.segment, self.confirm_empty, self.note = 0, [], False, ""
        self.mixer_start, self.last_navigation, self.power = None, None, None

    def run(self):
        """Returns True after every step was recorded."""
        try:
            self.ctl("push-exit off")
            log = json.loads(self.ctl("surface-log 0"))
            cursor, overflows = log["newest"], log["diagnostic_overflows"]
            deadline, shown, dirty = self.clock() + self.seconds, None, True
            self._start_step()
            self.report({"event": "ready", "steps": len(self.steps)})
            while self.index < len(self.steps):
                if self.clock() > deadline:
                    self.report({"event": "timeout", "index": self.index})
                    return False
                log = json.loads(self.ctl("surface-log %d" % cursor))
                if log["newest"] < cursor:
                    raise RuntimeError("sample-base restarted during capture")
                packets = log["packets"]
                if (packets and packets[0][0] > cursor + 1) or log["diagnostic_overflows"] != overflows:
                    self.note, dirty = "Data lost   use Pad 2 to redo", True
                    self.report({"event": "lost", "index": self.index})
                overflows, cursor = log["diagnostic_overflows"], log["next"]
                for sequence, timestamp, size, data in packets:
                    dirty = self._packet(sequence, timestamp, size, data) or dirty
                    if self.index == len(self.steps):
                        break
                if self.index < len(self.steps) and self.steps[self.index].get("source") == "power":
                    dirty = self._poll_power() or dirty
                if self.index < len(self.steps) and dirty and (shown is None or self.clock() - shown > 0.08):
                    self._show()
                    shown, dirty = self.clock(), False
                if len(packets) < 256:
                    self.sleep(0.02)
            self.report({"event": "complete", "steps": len(self.steps)})
            self.ctl("prompt CONTROL CAPTURE|Complete|All %d steps recorded||Wheel push returns to stock again"
                     % len(self.steps))
            self.sleep(self.finish_seconds)
            return True
        finally:
            for command in ("prompt", "push-exit on"):
                try:
                    self.ctl(command)
                except (OSError, RuntimeError):
                    pass

    def _packet(self, sequence, timestamp, size, data):
        if len(data) == 3 and data[0] == 0x99 and data[2] > 0 and data[1] in self.navigation:
            now = self.clock()
            if self.last_navigation is not None and now - self.last_navigation < self.debounce:
                return False
            self.last_navigation = now
            self._navigate(self.navigation[data[1]])
            return True
        if len(data) == 3 and data[0] in (0x89, 0x99, 0xa9) and data[1] in self.pads:
            return False
        self.segment.append({"sequence": sequence, "timestamp_ns": timestamp, "size": size, "bytes": data})
        self.confirm_empty, self.note = False, ""
        return True

    def _navigate(self, action):
        step = self.steps[self.index]
        if action == "next":
            if not self.segment and not step.get("optional") and not self.confirm_empty:
                self.confirm_empty, self.note = True, "Nothing captured   Pad 1 again to skip"
                return
            record = dict(step, index=self.index, packets=self.segment, identities=summarize(self.segment))
            if step.get("mixer"):
                record["mixer_changes"] = mixer_changes(self.mixer_start, mixer_values(self.ctl))
            self.recorded[self.index] = record
            self.report(dict(record, event="step"))
            self.index += 1
            self.note = ""
        elif action == "redo":
            self.note = "Cleared   repeat " + step["label"]
        elif self.index > 0:
            self.index -= 1
            self.recorded.pop(self.index, None)
            self.report({"event": "back", "index": self.index})
            self.note = "Back   repeat this step"
        if self.index < len(self.steps):
            self._start_step()

    def _start_step(self):
        self.segment, self.confirm_empty = [], False
        if self.steps[self.index].get("mixer"):
            self.mixer_start = mixer_values(self.ctl)
        if self.steps[self.index].get("source") == "power":
            self.power = json.loads(self.ctl("power"))
            if not self.power["available"]:
                self.note = "Power key device not found"

    def _poll_power(self):
        """Records KEY_POWER changes; a press and release between polls are both kept."""
        state = json.loads(self.ctl("power"))
        presses = state["presses"] - self.power["presses"]
        changes = [True] * presses + ([False] if presses and not state["pressed"] else [])
        if not presses and state["pressed"] != self.power["pressed"]:
            changes = [state["pressed"]]
        self.power = state
        for pressed in changes:
            self.segment.append({"input": state["key"], "pressed": pressed, "device": state["device"],
                                 "timestamp_ns": state["timestamp_ns"]})
        if changes:
            self.confirm_empty, self.note = False, ""
        return bool(changes)

    def _show(self):
        step = self.steps[self.index]
        if self.note:
            status = self.note
        elif self.segment:
            entry = self.segment[-1]
            last = "%s %s" % (entry["input"], "down" if entry["pressed"] else "up") if "input" in entry else \
                " ".join("%02X" % b for b in entry["bytes"][:3])
            status = "Captured %d   last %s" % (len(self.segment), last)
        else:
            status = "Waiting for " + step["label"]
        header = "CONTROL CAPTURE   %d / %d" % (self.index + 1, len(self.steps))
        self.ctl("prompt " + "|".join([header, step["label"], step["detail"], status, FOOTER]))


class StreamControl:
    """`sample-base --ctl-stream` over one SSH connection."""

    def __init__(self, device):
        self.process = subprocess.Popen(
            ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "-p", str(device.port), "-i", device.key,
             "root@" + device.host, ACTIVE + "/sample-base --ctl-stream"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def __call__(self, command):
        if "\n" in command:
            raise ValueError("Control commands are single lines")
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        reply = self.process.stdout.readline().rstrip("\n")
        if not reply.startswith("OK"):
            raise RuntimeError("%s: %s" % (command.split(" ")[0], reply or "control stream closed"))
        return reply[3:]

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="192.168.155.1")
    parser.add_argument("--port", type=int, default=2222)
    parser.add_argument("--key", default="~/.ssh/mpc_sample_client_ed25519")
    parser.add_argument("--seconds", type=int, default=1500, help="give up after N seconds")
    parser.add_argument("--only", help="comma-separated control ids to capture, e.g. power,k1")
    args = parser.parse_args()
    steps = STEPS
    if args.only:
        wanted = set(args.only.split(","))
        unknown = wanted - {step["control"] for step in STEPS}
        if unknown:
            raise ValueError("Unknown control: " + ", ".join(sorted(unknown)))
        steps = [step for step in STEPS if step["control"] in wanted]
    stamp = time.strftime("%Y%m%d-%H%M%S")
    raw_path = ROOT / ".local" / ("control-capture-" + stamp + ".jsonl")
    summary_path = ROOT / ".local" / ("control-capture-" + stamp + ".json")
    raw_path.parent.mkdir(exist_ok=True)
    with raw_path.open("w") as raw:
        def report(event):
            raw.write(json.dumps(event) + "\n"); raw.flush()
            kind = event["event"]
            if kind == "ready":
                print("READY: follow the prompts on the MPC screen (%d steps)" % event["steps"], flush=True)
            elif kind == "step":
                found = "; ".join(map(describe, event["identities"])) or "no surface MIDI"
                print("%2d/%d %s %s: %s" % (event["index"] + 1, len(steps), event["label"], event["gesture"], found),
                      flush=True)
                if event.get("mixer_changes"):
                    print("      mixer: " + json.dumps(event["mixer_changes"]), flush=True)
            elif kind == "back":
                print("back to step %d" % (event["index"] + 1), flush=True)
            elif kind == "lost":
                print("WARNING: packets lost during step %d; redo it" % (event["index"] + 1), flush=True)
            elif kind == "timeout":
                print("Timed out at step %d" % (event["index"] + 1), flush=True)
        control = StreamControl(Device(args.host, args.port, args.key))
        capture = Capture(control, steps, pad_notes(), report, seconds=args.seconds)
        try:
            complete = capture.run()
        finally:
            control.close()
            summary_path.write_text(json.dumps({"complete": capture.index == len(steps), "steps": [
                {key: step.get(key) for key in ("index", "control", "label", "gesture", "identities", "mixer_changes")}
                for _, step in sorted(capture.recorded.items())]}, indent=1) + "\n")
            print("Raw events: " + str(raw_path.relative_to(ROOT)))
            print("Summary: " + str(summary_path.relative_to(ROOT)))
    return 0 if complete else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        raise SystemExit(str(error))
