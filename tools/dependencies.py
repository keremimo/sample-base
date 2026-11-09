#!/usr/bin/env python3
"""Fetch the pinned public JUCE dependency into ignored build storage."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
JUCE_REVISION = "d60b7a78030661574a615b586d9325ba7a7fa7dd"  # JUCE 9.0.5


def prepare():
    path = ROOT / "build/deps/JUCE"
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "--depth", "1", "--branch", "9.0.5",
                        "https://github.com/juce-framework/JUCE.git", str(path)], check=True)
    revision = subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
    if revision != JUCE_REVISION:
        raise RuntimeError("JUCE revision mismatch; remove build/deps/JUCE and rebuild")


if __name__ == "__main__":
    prepare()
