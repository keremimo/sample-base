#!/usr/bin/env python3
"""Stage sample-base in MPC RAM and switch back to the existing stock service."""
import argparse
import hashlib
from pathlib import Path
import shlex
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parent
STOCK_SHA256 = "b6584cba76e416e432ad202509099d34373dc2c3d6a8fb625f5678bf0193b508"
UNIT_PATH = "/run/systemd/system/sample-base.service"
ACTIVE = "/run/sample-base"


def service_unit(release, seconds=0):
    if not release.startswith("/run/sample-base-release-") or not release.removeprefix("/run/sample-base-release-").isalnum():
        raise ValueError("Invalid RAM release path")
    if not isinstance(seconds, int) or seconds < 0:
        raise ValueError("Invalid session duration")
    return f"""[Unit]
Description=sample-base standalone JUCE instrument

[Service]
Type=exec
ExecStartPre=/bin/systemctl stop ac50.service
ExecStart={release}/sample-base
ExecStopPost=/bin/sh {release}/restore-stock.sh
Restart=no
TimeoutStartSec=30
TimeoutStopSec=10
RuntimeMaxSec={seconds if seconds else 'infinity'}
UMask=0077
"""


class Device:
    def __init__(self, host="192.168.155.1", port=2222, key="~/.ssh/mpc_sample_client_ed25519"):
        self.host, self.port, self.key = host, port, str(Path(key).expanduser())

    def run(self, command, *, input=None, check=True):
        result = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
                                 "-p", str(self.port), "-i", self.key, "root@" + self.host, command],
                                input=input, text=True, capture_output=True, timeout=45)
        if check and result.returncode:
            raise RuntimeError(result.stderr.strip() or result.stdout.strip() or "MPC command failed")
        return result

    def check_identity(self):
        machine = self.run("uname -m").stdout.strip()
        digest = self.run("sha256sum '/usr/bin/MPC Sample'").stdout.split()[0]
        if machine != "aarch64" or digest != STOCK_SHA256:
            raise RuntimeError("This proof of concept requires MPC Sample AC50 firmware 1.3.0.12")

    def stage(self):
        self.check_identity()
        files = {"sample-base": ROOT / "build/sample-base",
                 "restore-stock.sh": ROOT / "install/restore-stock.sh"}
        for path in files.values():
            if not path.is_file():
                raise RuntimeError("Build first with ./build.sh")
        release = "/run/sample-base-release-" + uuid.uuid4().hex[:12]
        self.run("mkdir -m 700 " + release)
        sums = []
        for name, path in files.items():
            subprocess.run(["scp", "-O", "-q", "-o", "BatchMode=yes", "-P", str(self.port), "-i", self.key,
                            str(path), "root@" + self.host + ":" + release + "/" + name], check=True)
            sums.append(hashlib.sha256(path.read_bytes()).hexdigest() + "  " + name)
        self.run("cd " + release + " && sha256sum -c -", input="\n".join(sums) + "\n")
        self.run("chmod 755 " + release + "/sample-base")
        # Resolve every shared-library dependency and test DSP while stock is
        # still running; neither operation opens hardware audio or the screen.
        self.run(release + "/sample-base --self-test " + release + "/test.wav")
        return release

    def start(self, seconds=0):
        if not isinstance(seconds, int) or seconds < 0:
            raise ValueError("Session duration must be zero or positive")
        release = self.stage()
        try:
            self.run("systemctl stop sample-base.service", check=False)
            self.run("ln -s " + release + " " + ACTIVE + ".next && mv -Tf " + ACTIVE + ".next " + ACTIVE)
            self.run("cat > " + UNIT_PATH, input=service_unit(release, seconds))
            self.run("systemctl daemon-reload && systemctl start sample-base.service")
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                state = self.run("systemctl is-active sample-base.service", check=False)
                if state.stdout.strip() in ("failed", "inactive"):
                    raise RuntimeError("Standalone app failed to start; stock app is being restored")
                ready = self.run(ACTIVE + "/sample-base --ctl status", check=False)
                if ready.returncode == 0:
                    print(ready.stdout.strip())
                    print("sample-base is running. Push returns to the stock app.")
                    return
                time.sleep(0.2)
            raise RuntimeError("Standalone app did not become ready; stock app is being restored")
        except Exception:
            self.stock()
            logs = self.run("journalctl -u sample-base.service -n 18 --no-pager", check=False)
            print(logs.stdout)
            raise

    def stock(self):
        # ExecStopPost restores stock even after a crash or forced termination.
        self.run("systemctl stop sample-base.service", check=False)
        self.run("systemctl start ac50.service")
        self.run("systemctl is-active --quiet ac50.service")
        print("Stock MPC app restored with its existing configuration.")

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
        subprocess.run(["scp", "-O", "-q", "-P", str(self.port), "-i", self.key,
                        "root@" + self.host + ":" + ACTIVE + "/screen.png", str(output)], check=True)
        print(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.155.1")
    parser.add_argument("--port", type=int, default=2222)
    parser.add_argument("--key", default="~/.ssh/mpc_sample_client_ed25519")
    commands = parser.add_subparsers(dest="command", required=True)
    start = commands.add_parser("start")
    start.add_argument("--seconds", type=int, default=0, help="automatically return to stock after N seconds")
    commands.add_parser("stock")
    commands.add_parser("status")
    ctl = commands.add_parser("ctl"); ctl.add_argument("action", nargs="+", help="running app command and arguments")
    screenshot = commands.add_parser("screenshot"); screenshot.add_argument("output", nargs="?", default=".local/screen.png")
    args = parser.parse_args()
    device = Device(args.host, args.port, args.key)
    if args.command == "start":
        device.start(args.seconds)
    elif args.command == "stock":
        device.stock()
    elif args.command == "status":
        device.status()
    elif args.command == "ctl":
        device.control(" ".join(args.action))
    else:
        device.screenshot(args.output)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as error:
        raise SystemExit(str(error))
