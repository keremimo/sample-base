#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
"""Stage sample-base in MPC RAM and switch back to the existing stock service."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parent
# The built app. An app in its own repository loads this module and points
# BINARY at its own build (docs/APPS.md); it is staged as "sample-base".
BINARY = ROOT / "build/sample-base"
# The stock firmwares an app may stand in for, by the SHA-256 of their stock
# app (install/sd-worker.sh checks the same list).
STOCK_FIRMWARES = {
    "b6584cba76e416e432ad202509099d34373dc2c3d6a8fb625f5678bf0193b508": "1.3.0.12",
    "2271252ffded26830546d4ee1852e39a2c1f4c574a40c47fba10b5a397727ec2": "1.4.0.7",
    "473fb7260e597e61cc55d9d30595eb9d9aa1304ff1b88960e633ad19457dc91a": "1.4.0.8",
}
# SSH: firmware 1.4's Developer Mode runs the stock SSH server on port 22 (no
# key or password); on 1.3.0.12 the card's autorun starts one on 2222 that
# takes the key below.
SSH_PORTS = (22, 2222)
UNIT_PATH = "/run/systemd/system/sample-base.service"
SWAP_DROPIN_DIR = UNIT_PATH + ".d"
SWAP_DROPIN = SWAP_DROPIN_DIR + "/swap.conf"
ACTIVE = "/run/sample-base"
RELEASE_PREFIX = "/run/sample-base-release-"
# Release folders in /run: stage() makes sample-base-release-<12 hex digits>,
# install/sd-worker.sh makes sample-base-release-sd<12 hex digits>.
RELEASE_NAME = re.compile(r"sample-base-release-(sd)?[0-9a-f]{12}")
# Room to keep on /run (RAM) beyond the staged files: systemd refuses
# daemon-reload with under 16 MiB free there; the self-test WAV is ~1 MB.
RUN_HEADROOM = 32 << 20
# Firmware 1.3.0.12 runs a card's autorun/autorun.sh only when autorun.check is
# this digest of autorun.json, autorun.sh and the salt (as mpc-sample-plugins
# found). Firmware 1.4 dropped that check: with Developer Mode on it offers to
# run an unsigned autorun.sh at the card's top, which install/top-autorun.sh
# points at autorun/autorun.sh.
AUTORUN_SALT = b"f7bed1c9-7e90-4731-9194-c996666b4331"
TOP_AUTORUN = ROOT / "install/top-autorun.sh"
TOP_MARKER = "# SAMPLE_BASE_TOP_AUTORUN"
SD_MARKER = "# SAMPLE_BASE_SD_START"
# install-sd's folder: inside the card's autorun folder, beside autorun.sh, so nothing is added at the card's top
# (the release zip's layout too). Installs before kept it at the card's top: install-sd moves that one in.
SD_FOLDER = "autorun/sample-base"
OLD_SD_FOLDER = "sample-base"
SD_COMMAND = ('/bin/sh "$(dirname -- "$0")/sample-base/sd-boot.sh" '
              '"$(dirname -- "$0")/sample-base" > /run/sample-base-sd.log 2>&1 || true')
PLUGINS_MARKER = "# MPC_PLUGINS_SD_START"
SD_FILES = ("sample-base", "restore-stock.sh", "sd-boot.sh", "sd-worker.sh", "sd-update.sh")


def autorun_check(manifest, script):
    """The autorun.check text for an autorun.json and autorun.sh."""
    return hashlib.sha256(manifest.encode() + script.encode() + AUTORUN_SALT).hexdigest()


def autorun_manifest(message="Start the app?"):
    """An autorun.json that runs autorun.sh without asking (firmware 1.3.0.12)."""
    script = {"path": "autorun.sh", "askQuestion": False, "message": message,
              "yesButtonText": "Start", "noButtonText": "Cancel"}
    return json.dumps({"value0": {"scripts": [script], "files": []}}, indent=2) + "\n"


def without_marker(script, marker, needle):
    """The script with a marker line and the line after it taken out."""
    lines = script.splitlines(keepends=True)
    for i, line in enumerate(lines):
        if line.rstrip() == marker:
            if i + 1 >= len(lines) or needle not in lines[i + 1]:
                raise RuntimeError("Unexpected autorun edit near " + marker + "; left for manual inspection")
            del lines[i:i + 2]
            break
    return "".join(lines)


def chain_autorun(script, remove_plugins=False):
    """The owner's autorun with ours run when it ends (its SSH setup goes first)."""
    if not script.startswith("#!"):
        raise RuntimeError("Existing autorun is not a shell script")
    script = without_marker(script, SD_MARKER, "sample-base/sd-boot.sh")
    if remove_plugins:
        script = without_marker(script, PLUGINS_MARKER, "MPC-Plugins/start-plugins.sh")
    elif PLUGINS_MARKER in script:
        raise RuntimeError("MPC-Plugins starts from this card too; pass --remove-plugins to replace it")
    lines = script.splitlines(keepends=True)
    lines.insert(1, SD_MARKER + "\n" + "trap '" + SD_COMMAND + "' EXIT\n")
    return "".join(lines)


def valid_app_name(app):
    return bool(app) and len(app) <= 32 and all(c.isascii() and (c.isalnum() or c in "-_") for c in app)


def stale_releases(names, active):
    """The /run paths to remove: every release folder among the names in
    /run (from stage() or an SD boot) except the active one."""
    if not active.startswith(RELEASE_PREFIX) or not active.removeprefix(RELEASE_PREFIX).isalnum():
        raise ValueError("No active release to keep: " + repr(active))
    keep = active.removeprefix("/run/")
    return sorted("/run/" + name for name in names if RELEASE_NAME.fullmatch(name) and name != keep)


def service_unit(release, seconds=0, app="demos"):
    if not release.startswith(RELEASE_PREFIX) or not release.removeprefix(RELEASE_PREFIX).isalnum():
        raise ValueError("Invalid RAM release path")
    if not isinstance(seconds, int) or seconds < 0:
        raise ValueError("Invalid session duration")
    if not valid_app_name(app):
        raise ValueError("Invalid app name")
    app_option = "" if app == "demos" else " --app " + app
    return f"""[Unit]
Description=sample-base standalone JUCE instrument

[Service]
Type=exec
ExecStartPre=/bin/systemctl stop ac50.service
ExecStart={release}/sample-base{app_option}
ExecStopPost=/bin/sh {release}/restore-stock.sh
Restart=no
TimeoutStartSec=30
TimeoutStopSec=10
RuntimeMaxSec={seconds if seconds else 'infinity'}
UMask=0077
"""


class Device:
    def __init__(self, host="192.168.155.1", port=None, key="~/.ssh/mpc_sample_client_ed25519"):
        """port None: the first of SSH_PORTS that lets root in."""
        self.host, self.key = host, str(Path(key).expanduser())
        self._port = port

    def options(self):
        """ssh and scp options: no prompts, a new MPC's host key taken on first use, the key when there is one."""
        options = ["-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "-o", "StrictHostKeyChecking=accept-new"]
        return options + (["-i", self.key] if Path(self.key).is_file() else [])

    @property
    def port(self):
        if self._port is None:
            for port in SSH_PORTS:
                probe = subprocess.run(["ssh", *self.options(), "-p", str(port), "root@" + self.host, "true"],
                                       capture_output=True, timeout=15)
                if probe.returncode == 0:
                    self._port = port
                    break
            else:
                raise RuntimeError("No SSH on the MPC at " + self.host + " (ports " + ", ".join(map(str, SSH_PORTS)) +
                                   "): on firmware 1.4, turn on Developer Mode")
        return self._port

    def run(self, command, *, input=None, check=True):
        result = subprocess.run(["ssh", *self.options(), "-p", str(self.port), "root@" + self.host, command],
                                input=input, text=True, capture_output=True, timeout=45)
        if check and result.returncode:
            raise RuntimeError(result.stderr.strip() or result.stdout.strip() or "MPC command failed")
        return result

    def copy(self, source, target):
        """scp of a local file to a path on the MPC."""
        subprocess.run(["scp", "-O", "-q", *self.options(), "-P", str(self.port), str(source),
                        "root@" + self.host + ":" + target], check=True)

    def firmware(self):
        """The stock firmware's version, from STOCK_FIRMWARES."""
        machine = self.run("uname -m").stdout.strip()
        digest = self.run("sha256sum '/usr/bin/MPC Sample'").stdout.split()[0]
        if machine != "aarch64" or digest not in STOCK_FIRMWARES:
            raise RuntimeError("Unknown stock firmware: this needs MPC Sample AC50 firmware " +
                               " or ".join(sorted(set(STOCK_FIRMWARES.values()))))
        return STOCK_FIRMWARES[digest]

    def check_identity(self):
        self.firmware()

    def stage(self):
        self.check_identity()
        files = {"sample-base": BINARY,
                 "restore-stock.sh": ROOT / "install/restore-stock.sh",
                 "sd-update.sh": ROOT / "install/sd-update.sh"}  # installs updates from the card
        for path in files.values():
            if not path.is_file():
                raise RuntimeError("Build first with ./build.sh")
        need = sum(path.stat().st_size for path in files.values()) + RUN_HEADROOM
        free = self.run_free()
        if free < need:
            stale = self.leftover_releases()
            raise RuntimeError(f"Only {free >> 20} MB free on the MPC's /run (RAM); staging needs {need >> 20} MB."
                               " Nothing was changed." +
                               (" Old releases to remove: rm -r " + " ".join(stale) if stale else ""))
        release = RELEASE_PREFIX + uuid.uuid4().hex[:12]
        self.run("mkdir -m 700 " + release)
        sums = []
        for name, path in files.items():
            self.copy(path, release + "/" + name)
            sums.append(hashlib.sha256(path.read_bytes()).hexdigest() + "  " + name)
        self.run("cd " + release + " && sha256sum -c -", input="\n".join(sums) + "\n")
        self.run("chmod 755 " + release + "/sample-base")
        # Resolve every shared-library dependency and test DSP while stock is
        # still running; neither operation opens hardware audio or the screen.
        self.run(release + "/sample-base --self-test " + release + "/test.wav")
        return release

    def run_free(self):
        """Bytes free on /run, the RAM disk releases are staged in."""
        df = self.run("df -Pk /run").stdout
        try:
            return int(df.splitlines()[-1].split()[3]) * 1024
        except (IndexError, ValueError):
            raise RuntimeError("Unexpected df output for /run: " + df.strip()) from None

    def leftover_releases(self):
        """stage()'s releases on the MPC other than the active one (none
        when no release is active)."""
        active = self.run("readlink " + ACTIVE, check=False).stdout.strip()
        try:
            return stale_releases(self.run("ls -1A /run").stdout.splitlines(), active)
        except ValueError:
            return []

    def remove_stale_releases(self, release):
        """Frees the RAM earlier starts took, once `release` runs. A failure
        only costs that RAM until the next start; the app keeps running."""
        try:
            active = self.run("readlink " + ACTIVE).stdout.strip()
            if active != release:
                raise RuntimeError(ACTIVE + " points at " + (active or "nothing") + ", not " + release)
            stale = stale_releases(self.run("ls -1A /run").stdout.splitlines(), active)
            if stale:
                self.run("rm -r " + " ".join(stale))
                print(f"Removed {len(stale)} old release(s) from /run; {self.run_free() >> 20} MB free.")
        except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as error:
            print("Old releases left on /run: " + str(error))

    def start(self, seconds=0, app="demos"):
        if not isinstance(seconds, int) or seconds < 0:
            raise ValueError("Session duration must be zero or positive")
        if not valid_app_name(app):
            raise ValueError("App names use letters, digits, - and _")
        release = self.stage()
        try:
            # Swap apps without the stock app in between: a runtime drop-in
            # clears ExecStopPost (restore-stock.sh) for this one stop.
            self.run("mkdir -p " + SWAP_DROPIN_DIR + " && printf '[Service]\\nExecStopPost=\\n' > " + SWAP_DROPIN
                     + " && systemctl daemon-reload && systemctl stop sample-base.service; rm -f " + SWAP_DROPIN, check=False)
            self.run("ln -s " + release + " " + ACTIVE + ".next && mv -Tf " + ACTIVE + ".next " + ACTIVE)
            self.run("cat > " + UNIT_PATH, input=service_unit(release, seconds, app))
            self.run("systemctl daemon-reload && systemctl start sample-base.service")
            # The first start after a boot rebinds the USB gadget for stems,
            # dropping this SSH link until the computer brings it back.
            deadline = time.monotonic() + 30
            while True:
                if time.monotonic() >= deadline:
                    raise RuntimeError("Standalone app did not become ready; stock app is being restored")
                state = self.run("systemctl is-active sample-base.service", check=False)
                if state.stdout.strip() in ("failed", "inactive"):
                    raise RuntimeError("Standalone app failed to start; stock app is being restored")
                ready = self.run(ACTIVE + "/sample-base --ctl status", check=False)
                if ready.returncode == 0:
                    print(ready.stdout.strip())
                    print("sample-base is running. Push returns to the stock app.")
                    break
                time.sleep(0.2)
        except Exception:
            self.stock()
            logs = self.run("journalctl -u sample-base.service -n 18 --no-pager", check=False)
            print(logs.stdout)
            raise
        # Outside the try: nothing here may bring the stock app back.
        self.remove_stale_releases(release)

    def stock(self):
        # ExecStopPost restores stock even after a crash or forced termination.
        self.run("systemctl stop sample-base.service", check=False)
        self.run("systemctl start ac50.service")
        self.run("systemctl is-active --quiet ac50.service")
        print("Stock MPC app restored with its existing configuration.")

    def card(self):
        """The SD card's folder: the one drive mounted under /media."""
        found = self.run("ls -d /media/*/ 2>/dev/null", check=False).stdout.split()
        if len(found) != 1:
            raise RuntimeError("Expected one SD card in the MPC")
        return found[0].rstrip("/")

    def card_autorun(self, autorun):
        """The card's autorun.json and autorun.sh, checked, and True; a new pair and False when it has none."""
        q = shlex.quote
        if self.run("[ -e " + q(autorun + "/autorun.sh") + " ]", check=False).returncode:
            return autorun_manifest(), "#!/bin/sh\n", False
        manifest = self.run("cat " + q(autorun + "/autorun.json")).stdout
        script = self.run("cat " + q(autorun + "/autorun.sh")).stdout
        if [entry.get("path") for entry in json.loads(manifest)["value0"]["scripts"]] != ["autorun.sh"]:
            raise RuntimeError("Unsupported SD autorun layout; nothing changed")
        if self.run("cat " + q(autorun + "/autorun.check")).stdout.strip() != autorun_check(manifest, script):
            raise RuntimeError("The card's autorun.check does not match its autorun; nothing changed")
        return manifest, script, True

    def top_autorun(self, card):
        """True when the card's top autorun.sh is install/top-autorun.sh's, False when there is none.
        Refuses another program's."""
        result = self.run("cat " + shlex.quote(card + "/autorun.sh"), check=False)
        if result.returncode:
            return False
        if TOP_MARKER not in result.stdout.splitlines():
            raise RuntimeError("The card's top autorun.sh is not sample-base's; nothing changed")
        return True

    def write_autorun(self, autorun, manifest, script):
        q = shlex.quote
        self.run("cat > " + q(autorun + "/autorun.sh.next"), input=script)
        self.run("cat > " + q(autorun + "/autorun.check.next"), input=autorun_check(manifest, script))
        self.run("sync && mv " + q(autorun + "/autorun.sh.next") + " " + q(autorun + "/autorun.sh") +
                 " && mv " + q(autorun + "/autorun.check.next") + " " + q(autorun + "/autorun.check") + " && sync")

    def install_sd(self, app, remove_plugins=False):
        """Puts the app on the card and starts it at every boot, after the card's own autorun."""
        if not valid_app_name(app):
            raise ValueError("App names use letters, digits, - and _")
        firmware = self.firmware()
        sources = {"sample-base": BINARY, "restore-stock.sh": ROOT / "install/restore-stock.sh",
                   "sd-boot.sh": ROOT / "install/sd-boot.sh", "sd-worker.sh": ROOT / "install/sd-worker.sh",
                   "sd-update.sh": ROOT / "install/sd-update.sh"}
        for path in sources.values():
            if not path.is_file():
                raise RuntimeError("Build first with ./build.sh")
        q = shlex.quote
        card = self.card()
        folder, autorun, old = card + "/" + SD_FOLDER, card + "/autorun", card + "/" + OLD_SD_FOLDER
        manifest, script, existing = self.card_autorun(autorun)
        chained = chain_autorun(script, remove_plugins)
        # Firmware 1.4 runs only the card's top autorun.sh; 1.3.0.12 ignores it, so it is left off such cards.
        top = firmware != "1.3.0.12"
        if top:
            self.top_autorun(card)
        self.move_old_sd_folder(old, folder)
        # The files first, checked, then the autorun: a cut-off install never starts half an app.
        self.run("mkdir -p " + q(folder + "/autorun-backup"))
        if existing and self.run("ls " + q(folder + "/autorun-backup/autorun.sh"), check=False).returncode:
            self.run("cp " + " ".join(q(autorun + "/" + n) for n in ("autorun.sh", "autorun.json", "autorun.check")) +
                     " " + q(folder + "/autorun-backup") + "/")
        sums = []
        for name, path in sources.items():
            self.copy(path, folder + "/" + name)
            sums.append(hashlib.sha256(path.read_bytes()).hexdigest() + "  " + name)
        self.run("cat > " + q(folder + "/SHA256SUMS"), input="\n".join(sums) + "\n")
        self.run("cat > " + q(folder + "/app"), input=app + "\n")
        self.run("cd " + q(folder) + " && sha256sum -c SHA256SUMS && sync")
        if not existing:
            self.run("cat > " + q(autorun + "/autorun.json"), input=manifest)
        self.write_autorun(autorun, manifest, chained)
        if top:
            self.run("cat > " + q(card + "/autorun.sh.next") + " && sync && mv " + q(card + "/autorun.sh.next") + " " +
                     q(card + "/autorun.sh") + " && sync", input=TOP_AUTORUN.read_text())
        print("Installed on the card: " + app + " starts at every boot; leaving it returns to stock until the next one.")
        if top:
            print("Firmware " + firmware + ": with Developer Mode on, the stock app asks to run the card's autorun.sh"
                  " each time it starts; Yes starts " + app + ".")
        if existing:
            print("Previous autorun kept in " + folder + "/autorun-backup.")

    def move_old_sd_folder(self, old, folder):
        """An install from before, at the card's top, into the autorun folder: renamed whole when nothing is there
        yet (its autorun backup and anything else in it go along); else its backup when the new folder has none, its
        installed files removed, and the folder itself when that leaves it empty."""
        q = shlex.quote
        if self.run("[ -d " + q(old) + " ]", check=False).returncode:
            return
        if self.run("[ -e " + q(folder) + " ]", check=False).returncode:
            self.run("mv " + q(old) + " " + q(folder) + " && sync")
            print("Moved " + old + " to " + folder + ".")
            return
        self.run("[ -e " + q(folder + "/autorun-backup") + " ] || [ ! -d " + q(old + "/autorun-backup") + " ] || mv " +
                 q(old + "/autorun-backup") + " " + q(folder + "/"))
        self.run("cd " + q(old) + " && rm -f " + " ".join(SD_FILES) + " SHA256SUMS app && cd / && (rmdir " + q(old) +
                 " 2>/dev/null || true) && sync")
        if not self.run("[ -d " + q(old) + " ]", check=False).returncode:
            print("Left " + old + ": it holds files install-sd did not put there.")

    def uninstall_sd(self):
        q = shlex.quote
        card = self.card()
        autorun = card + "/autorun"
        manifest = self.run("cat " + q(autorun + "/autorun.json")).stdout
        script = self.run("cat " + q(autorun + "/autorun.sh")).stdout
        plain = without_marker(script, SD_MARKER, "sample-base/sd-boot.sh")
        if plain == script:
            print("The card does not start sample-base; nothing changed.")
            return
        self.write_autorun(autorun, manifest, plain)
        if self.top_autorun(card):
            self.run("rm -f " + q(card + "/autorun.sh") + " && sync")
        for folder in (card + "/" + SD_FOLDER, card + "/" + OLD_SD_FOLDER):
            self.run("[ ! -d " + q(folder) + " ] || (cd " + q(folder) + " && rm -f " + " ".join(SD_FILES) +
                     " SHA256SUMS app && sync)")
        print("sample-base no longer starts from the card (its autorun backup stays in " + SD_FOLDER + "/autorun-backup).")

    def control(self, command):
        print(self.run(ACTIVE + "/sample-base --ctl " + shlex.quote(command)).stdout.strip())

    def status(self):
        for name in ("sample-base", "ac50"):
            result = self.run("systemctl is-active " + name + ".service", check=False)
            print(name + ": " + result.stdout.strip())
            if name == "sample-base" and result.stdout.strip() == "active":
                self.control("status")

    def screenshot(self, output):
        self.control("screenshot")
        time.sleep(0.4)
        output = Path(output)
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["scp", "-O", "-q", *self.options(), "-P", str(self.port),
                        "root@" + self.host + ":" + ACTIVE + "/screen.png", str(output)], check=True)
        print(output)


def main(default_app="demos"):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.155.1")
    parser.add_argument("--port", type=int, help="SSH port (default: 22, else 2222 for firmware 1.3.0.12)")
    parser.add_argument("--key", default="~/.ssh/mpc_sample_client_ed25519")
    commands = parser.add_subparsers(dest="command", required=True)
    start = commands.add_parser("start")
    start.add_argument("--seconds", type=int, default=0, help="automatically return to stock after N seconds")
    start.add_argument("--app", default=default_app, help=f"which app to run (default: {default_app})")
    commands.add_parser("stock")
    install = commands.add_parser("install-sd", help="start an app from the SD card at every boot")
    install.add_argument("--app", default=default_app)
    install.add_argument("--remove-plugins", action="store_true", help="stop MPC-Plugins starting from the card")
    commands.add_parser("uninstall-sd")
    commands.add_parser("status")
    ctl = commands.add_parser("ctl"); ctl.add_argument("action", nargs="+", help="running app command and arguments")
    screenshot = commands.add_parser("screenshot"); screenshot.add_argument("output", nargs="?", default=".local/screen.png")
    args = parser.parse_args()
    device = Device(args.host, args.port, args.key)
    if args.command == "start":
        device.start(args.seconds, args.app)
    elif args.command == "stock":
        device.stock()
    elif args.command == "install-sd":
        device.install_sd(args.app, args.remove_plugins)
    elif args.command == "uninstall-sd":
        device.uninstall_sd()
    elif args.command == "status":
        device.status()
    elif args.command == "ctl":
        device.control(" ".join(args.action))
    else:
        device.screenshot(args.output)


def cli(default_app="demos"):
    try:
        main(default_app)
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as error:
        raise SystemExit(str(error))


if __name__ == "__main__":
    cli()
