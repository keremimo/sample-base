#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Guided measurement of the MPC Sample's button LED values.

A running sample-base session lights one button LED at a time with one
value (channel-1 CC numbered like the button note, as the stock app sends).
The pads light as an answer palette; tap the pad whose colour matches the
button. Pad 1 means the button stayed dark and pad 4 goes back one step.
Answers are recorded from the surface log and written to ignored `.local/`.

Palette, by printed pad number:
  13 dim green    14 bright green
   9 dim red      10 bright red      11 red + white / pink
   5 dim white     6 bright white
   1 off                              4 back
"""
import argparse
import json
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from device import Device  # noqa: E402
from tools.capture_controls import StreamControl, pad_notes  # noqa: E402

SURFACE_NAME = "Control Surface"
# Button LEDs in panel order: name and CC number (= button note).
LEDS = [("Sample", 6), ("SEQ", 7), ("Pad FX", 1), ("Knob FX", 0), ("Pad Bank", 35),
        ("Erase", 9), ("Note Repeat", 11), ("Chop", 75), ("Mute", 43), ("Loop", 74),
        ("16 Levels", 40), ("Sample Select", 36), ("Tap Tempo", 53),
        ("Sample Record", 73), ("Seq Record", 80), ("Stop", 81), ("Play", 82)]
VALUES = [1, 2, 3, 4]
# Printed pad number -> (answer, RGB shown on that pad).
PALETTE = {1: ("off", (0, 0, 0)), 5: ("dim white", (10, 10, 10)), 6: ("bright white", (127, 127, 127)),
           9: ("dim red", (14, 0, 0)), 10: ("bright red", (127, 0, 0)), 11: ("red + white", (127, 45, 55)),
           13: ("dim green", (0, 14, 0)), 14: ("bright green", (0, 127, 0))}
BACK = 4
STOCK_LED_CCS = [n for _, n in LEDS] + [49, 55, 54] + list(range(57, 65))


def cc(address, number, value):
    return "midi-send %s b0 %02x %02x" % (address, number, value)


def pad(address, index, rgb):
    return "midi-send %s f0 47 7f 5c 65 00 04 %02x %02x %02x %02x f7" % ((address, index) + tuple(rgb))


def send(ctl, command):
    """Retries while the bounded output queue drains."""
    for _ in range(100):
        try:
            return ctl(command)
        except RuntimeError as error:
            if "full output queue" not in str(error):
                raise
            time.sleep(0.01)
    raise RuntimeError("surface output queue stayed full")


def surface_address(ctl):
    ports = json.loads(ctl("midi"))["state"]["ports"]
    for port in ports:
        if port.get("surface") and port.get("output"):
            return port["address"]
    raise RuntimeError("No surface output")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--only", help="comma-separated LED names, e.g. Stop,Play")
    args = parser.parse_args()
    leds = LEDS if not args.only else [led for led in LEDS if led[0] in args.only.split(",")]
    steps = [(name, number, value) for name, number in leds for value in VALUES]
    notes = pad_notes()
    pad_of_note = {note: n + 1 for n, note in enumerate(notes)}
    ctl = StreamControl(Device())
    address = surface_address(ctl)
    results, index, cursor, last_tap = {}, 0, json.loads(ctl("surface-log 0"))["newest"], 0.0
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out = ROOT / ".local" / ("led-probe-" + stamp + ".json")
    try:
        for number in STOCK_LED_CCS:
            send(ctl, cc(address, number, 0))
        for n in range(17):
            send(ctl, pad(address, n, PALETTE.get(n + 1, (None, (0, 0, 0)))[1]))
        shown = None
        while index < len(steps):
            name, number, value = steps[index]
            if shown != index:
                if shown is not None:
                    send(ctl, cc(address, steps[shown][1], 0))
                send(ctl, cc(address, number, value))
                ctl("prompt LED PROBE  %d / %d|%s|value %d|Tap matching pad   1 off   4 back|"
                    "5/6 white  9/10 red  11 pink  13/14 green" % (index + 1, len(steps), name, value))
                shown = index
            log = json.loads(ctl("surface-log %d" % cursor))
            cursor = log["next"]
            for _, _, _, data in log["packets"]:
                if len(data) != 3 or data[0] != 0x99 or data[2] == 0 or data[1] not in pad_of_note:
                    continue
                if time.monotonic() - last_tap < 0.3:
                    continue
                last_tap, tapped = time.monotonic(), pad_of_note[data[1]]
                if tapped == BACK and index > 0:
                    index -= 1
                    results.pop(index, None)
                    print("back to %d" % (index + 1), flush=True)
                elif tapped in PALETTE:
                    answer = PALETTE[tapped][0]
                    results[index] = {"led": name, "cc": number, "value": value, "seen": answer}
                    print("%2d/%d %-13s CC %-3d value %d: %s" % (index + 1, len(steps), name, number, value, answer),
                          flush=True)
                    index += 1
                break
            time.sleep(0.03)
        ctl("prompt LED PROBE|Complete|%d answers recorded||" % len(results))
        time.sleep(2)
    finally:
        out.parent.mkdir(exist_ok=True)
        out.write_text(json.dumps([results[k] for k in sorted(results)], indent=1) + "\n")
        try:
            for number in STOCK_LED_CCS:
                send(ctl, cc(address, number, 0))
            for n in range(17):
                send(ctl, pad(address, n, (0, 0, 0)))
            ctl("prompt")
        finally:
            ctl.close()
        print("Answers: " + str(out.relative_to(ROOT)))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        raise SystemExit(str(error))
