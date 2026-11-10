#!/usr/bin/env python3
"""Fetch the pinned public dependencies into ignored build storage: JUCE, and
whatever an app's own src/<app>/dependencies.py fetches.

An app in its own repository calls prepare() with its own root, so JUCE and
its files land in that repository's build/."""
from pathlib import Path
import importlib.util
import subprocess

ROOT = Path(__file__).resolve().parents[1]
JUCE_REVISION = "d60b7a78030661574a615b586d9325ba7a7fa7dd"  # JUCE 9.0.5


def prepare(root=ROOT):
    root = Path(root)
    path = root / "build/deps/JUCE"
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "--depth", "1", "--branch", "9.0.5",
                        "https://github.com/juce-framework/JUCE.git", str(path)], check=True)
    revision = subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
    if revision != JUCE_REVISION:
        raise RuntimeError("JUCE revision mismatch; remove build/deps/JUCE and rebuild")
    # An app may fetch its own pinned files (fonts, data) with a prepare(root).
    for script in sorted((root / "src").glob("*/dependencies.py")):
        spec = importlib.util.spec_from_file_location(script.parent.name + "_dependencies", script)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.prepare(root)


if __name__ == "__main__":
    prepare()
