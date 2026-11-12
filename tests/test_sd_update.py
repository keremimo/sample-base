# SPDX-License-Identifier: AGPL-3.0-only
"""Tests install/sd-update.sh against a fake card, /run and systemctl, with
zips signed by a test key, and src/drivers/sd_update.cpp."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest
import zipfile

from test_platform import compile_fixture

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "install/sd-update.sh"


def openssl_with_ed25519():
    tool = shutil.which("openssl")
    if not tool:
        return None
    version = subprocess.run([tool, "version"], capture_output=True, text=True).stdout
    return tool if version.startswith("OpenSSL 3") else None


OPENSSL = openssl_with_ed25519()
TOOLS = OPENSSL and shutil.which("unzip") and shutil.which("sha256sum")

# Fake app binary: --self-test fails if $FIX/self-test-fails exists, --ctl
# fails if $FIX/never-ready exists.
FAKE_APP = """#!/bin/sh
case "$1" in
    --self-test) [ ! -e "$FIX/self-test-fails" ] || exit 1; : > "$2" ;;
    --ctl) [ ! -e "$FIX/never-ready" ] || exit 1 ;;
esac
"""
# Fake sd-worker.sh: copies the card's binary to a RAM release and makes it active.
FAKE_WORKER = """#!/bin/sh
set -eu
R="$FIX/run/sample-base-release-sdnew"
mkdir -p "$R"
cp "$1/sample-base" "$1/restore-stock.sh" "$R/"
chmod 755 "$R/sample-base"
rm -f "$FIX/run/sample-base"
ln -s "$R" "$FIX/run/sample-base"
echo "worker $1" >> "$FIX/calls"
"""


class Fixture:
    def __init__(self, temp):
        self.root = Path(temp)
        self.run_dir = self.root / "run"
        (self.run_dir / "systemd/system").mkdir(parents=True)
        self.card = self.root / "card"
        self.card.mkdir()
        self.calls = self.root / "calls"
        self.key = self.root / "key.pem"
        self.public = self.root / "public.pem"
        subprocess.run([OPENSSL, "genpkey", "-algorithm", "ed25519", "-out", str(self.key)], check=True, capture_output=True)
        subprocess.run([OPENSSL, "pkey", "-in", str(self.key), "-pubout", "-out", str(self.public)], check=True)
        systemctl = self.tool("systemctl", 'printf "%s\\n" "$*" >> "$FIX/calls"\n'
                                           '[ "$1" != is-active ] || cat "$FIX/state" 2>/dev/null || echo active\n')
        move = self.tool("move-T", 'rm -f "$2"\nmv "$1" "$2"\n')  # macOS mv has no -T
        source = SCRIPT.read_text()
        for old, new in (("RUN=/run\n", "RUN=" + shlex.quote(str(self.run_dir)) + "\n"),
                         ("SYSTEMCTL=/bin/systemctl\n", "SYSTEMCTL=" + shlex.quote(str(systemctl)) + "\n"),
                         ("OPENSSL=openssl\n", "OPENSSL=" + shlex.quote(OPENSSL) + "\n"),
                         ("READY_SECONDS=30\n", "READY_SECONDS=4\n"),
                         ("mv -Tf ", shlex.quote(str(move)) + " ")):
            assert old in source, old
            source = source.replace(old, new)
        self.script = self.root / "sd-update.sh"
        self.script.write_text(source)
        # The running app's release in RAM.
        self.old = self.run_dir / "sample-base-release-0123456789ab"
        self.old.mkdir()
        (self.old / "sample-base").write_text(FAKE_APP)
        (self.old / "sample-base").chmod(0o755)
        (self.old / "restore-stock.sh").write_text("#!/bin/sh\n")
        (self.run_dir / "sample-base").symlink_to(self.old)
        # Releases left by earlier starts: an SD boot and a device.py start.
        self.stale = [self.run_dir / "sample-base-release-sd00112233aabb", self.run_dir / "sample-base-release-ffeeddccbbaa"]
        for release in self.stale:
            release.mkdir()
            (release / "sample-base").write_text("old\n")

    def tool(self, name, body):
        path = self.root / name
        path.write_text("#!/bin/sh\n" + body)
        path.chmod(0o755)
        return path

    def release(self, version="v0.2.0", package="melodee", app="melodee", key=None, extra=None, listed=None,
                autorun_sh=None, tamper=None):
        """A release zip like tools/make_release.py makes, signed with `key`
        (the fixture key by default). `extra`: files added to the zip but
        not to the list; `listed`: paths to list instead of the real ones;
        `tamper`: a file changed after signing."""
        pkg = f"autorun/{package}"
        files = {f"{pkg}/sample-base": FAKE_APP, f"{pkg}/restore-stock.sh": "#!/bin/sh\n",
                 f"{pkg}/sd-boot.sh": "#!/bin/sh\n", f"{pkg}/sd-worker.sh": FAKE_WORKER,
                 f"{pkg}/sd-update.sh": "#!/bin/sh\n", f"{pkg}/app": app + "\n", f"{pkg}/VERSION": version + " 0123abcd\n",
                 f"{pkg}/README.txt": "read me\n", f"{pkg}/licenses/LICENSE.txt": "licence\n",
                 "autorun/autorun.sh": autorun_sh or f'#!/bin/sh\n/bin/sh "$D/{package}/sd-boot.sh" "$D/{package}"\n',
                 "autorun/autorun.json": "{}\n", "autorun/autorun.check": "0" * 64 + "\n"}
        sums = "".join(hashlib.sha256(files[f"{pkg}/{n}"].encode()).hexdigest() + "  " + n + "\n"
                       for n in ("sample-base", "restore-stock.sh", "sd-boot.sh", "sd-worker.sh", "sd-update.sh"))
        files[f"{pkg}/SHA256SUMS"] = sums
        names = listed if listed is not None else sorted(files)
        listing = "".join(hashlib.sha256(files.get(n, "").encode()).hexdigest() + "  " + n + "\n" for n in names)
        list_path = self.root / "list"
        list_path.write_text(listing)
        signature = self.root / "list.sig"
        subprocess.run([OPENSSL, "pkeyutl", "-sign", "-inkey", str(key or self.key), "-rawin",
                        "-in", str(list_path), "-out", str(signature)], check=True)
        files.update(extra or {})
        if tamper:
            files[tamper] += "changed\n"
        zip_path = self.card / f"Melodee-MPC-Sample-{version}.zip"
        with zipfile.ZipFile(zip_path, "w") as archive:
            for name, text in files.items():
                archive.writestr(name, text)
            archive.writestr(f"{pkg}/RELEASE.sha256", listing)
            archive.writestr(f"{pkg}/RELEASE.sha256.sig", signature.read_bytes())
        return zip_path

    def installed(self, version="v0.1.0"):
        """A card with an earlier release installed."""
        package = self.card / "autorun/melodee"
        package.mkdir(parents=True)
        (package / "VERSION").write_text(version + " old\n")
        (package / "sample-base").write_text("old app\n")
        (self.card / "autorun/autorun.sh").write_text('#!/bin/sh\n/bin/sh "$D/melodee/sd-boot.sh" "$D/melodee"\n# old\n')
        (self.card / "autorun/autorun.json").write_text("{old}\n")
        (self.card / "autorun/autorun.check").write_text("old check\n")

    def check(self, zip_path, package="melodee", key=None):
        return subprocess.run(["sh", str(self.script), "check", str(zip_path), package, str(key or self.public)],
                              capture_output=True, text=True, env={**os.environ, "FIX": str(self.root)})

    def install(self, zip_path, app="melodee"):
        result = subprocess.run(["sh", str(self.script), "install", str(zip_path), str(self.card), "melodee", app,
                                 str(self.public)], capture_output=True, text=True, env={**os.environ, "FIX": str(self.root)},
                                timeout=60)
        status = dict(line.split("=", 1) for line in (self.run_dir / "sample-base-update.status").read_text().splitlines())
        return result, status

    def call_lines(self):
        return self.calls.read_text().splitlines() if self.calls.exists() else []


@unittest.skipUnless(TOOLS, "requires OpenSSL 3, unzip and sha256sum")
class SdUpdateScriptTests(unittest.TestCase):
    def test_check_accepts_only_signed_releases(self):
        with tempfile.TemporaryDirectory() as temp:
            f = Fixture(temp)
            zip_path = f.release("v0.2.0")
            result = f.check(zip_path)
            self.assertEqual((result.returncode, result.stdout), (0, "v0.2.0\n"))
            self.assertEqual(f.check(zip_path, package="other").returncode, 1)
            other = Path(temp) / "other.pem"
            subprocess.run([OPENSSL, "genpkey", "-algorithm", "ed25519", "-out", str(other)], check=True, capture_output=True)
            self.assertEqual(f.check(f.release("v0.3.0", key=other)).returncode, 1)  # wrong key
            self.assertEqual(f.check(f.release("v0.4.0", tamper="autorun/melodee/VERSION")).returncode, 1)
            not_zip = f.card / "notes.zip"
            not_zip.write_text("not a zip")
            self.assertEqual(f.check(not_zip).returncode, 1)
            self.assertEqual(list(f.run_dir.glob("sample-base-check.*")), [])  # temp folder removed
            self.assertFalse(f.calls.exists())

    def test_install_writes_card_and_restarts_app_without_stock(self):
        with tempfile.TemporaryDirectory() as temp:
            f = Fixture(temp)
            f.installed()
            zip_path = f.release("v0.2.0", extra={"autorun/melodee/unlisted.sh": "#!/bin/sh\n"})
            result, status = f.install(zip_path)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(status, {"state": "done", "version": "v0.2.0", "error": ""})
            package = f.card / "autorun/melodee"
            self.assertEqual((package / "VERSION").read_text(), "v0.2.0 0123abcd\n")
            self.assertEqual((package / "licenses/LICENSE.txt").read_text(), "licence\n")
            self.assertFalse((package / "unlisted.sh").exists())  # only listed files are extracted
            self.assertFalse((package / "RELEASE.sha256").exists())
            self.assertEqual((f.card / "autorun/melodee.prev/VERSION").read_text(), "v0.1.0 old\n")
            self.assertFalse((f.card / "autorun/melodee.next").exists())
            self.assertEqual((f.card / "autorun/autorun.json").read_text(), "{}\n")
            self.assertFalse(zip_path.exists())  # zip deleted after install
            calls = f.call_lines()
            stop = calls.index("stop sample-base.service")
            self.assertEqual(calls[stop - 1], "daemon-reload")  # drop-in that skips restore-stock.sh
            self.assertIn(f"worker {package}", calls)
            self.assertNotIn("start ac50.service", calls)
            self.assertFalse((f.run_dir / "systemd/system/sample-base.service.d/swap.conf").exists())
            self.assertEqual(os.readlink(f.run_dir / "sample-base"), str(f.run_dir / "sample-base-release-sdnew"))
            self.assertFalse(f.old.exists())  # previous and stale releases removed
            self.assertEqual(sorted(f.run_dir.glob("sample-base-release-*")), [f.run_dir / "sample-base-release-sdnew"])
            self.assertEqual(list(f.run_dir.glob("sample-base-update.*")), [f.run_dir / "sample-base-update.status"])

    def test_install_on_card_without_autorun(self):
        with tempfile.TemporaryDirectory() as temp:
            f = Fixture(temp)
            result, status = f.install(f.release("v0.2.0"))
            self.assertEqual(status["state"], "done", result.stderr)
            self.assertTrue((f.card / "autorun/melodee/sample-base").exists())
            self.assertFalse((f.card / "autorun/melodee.prev").exists())

    def test_rejected_zip_changes_nothing(self):
        cases = {
            "signature": dict(key="other"),
            "damaged": dict(tamper="autorun/melodee/sample-base"),
            "other-app": dict(app="demos"),
            "signature ": dict(listed=["autorun/melodee/VERSION", "autorun/../escape"]),
        }
        for error, options in cases.items():
            with self.subTest(error=error), tempfile.TemporaryDirectory() as temp:
                f = Fixture(temp)
                f.installed()
                if options.get("key") == "other":
                    options["key"] = Path(temp) / "other.pem"
                    subprocess.run([OPENSSL, "genpkey", "-algorithm", "ed25519", "-out", str(options["key"])],
                                   check=True, capture_output=True)
                zip_path = f.release("v0.2.0", **options)
                result, status = f.install(zip_path)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(status["state"], "failed")
                self.assertEqual(status["error"], error.strip())
                self.assertEqual((f.card / "autorun/melodee/VERSION").read_text(), "v0.1.0 old\n")
                self.assertTrue(zip_path.exists())
                self.assertEqual(f.call_lines(), [])  # app not stopped
                self.assertTrue(all(release.exists() for release in [f.old, *f.stale]))  # /run not changed
                self.assertFalse((Path(temp) / "escape").exists())
                self.assertEqual(list(f.run_dir.glob("sample-base-update.*")), [f.run_dir / "sample-base-update.status"])

    def test_card_with_other_autorun_is_not_changed(self):
        with tempfile.TemporaryDirectory() as temp:
            f = Fixture(temp)
            (f.card / "autorun").mkdir()
            (f.card / "autorun/autorun.sh").write_text("#!/bin/sh\n# boot menu\n")
            result, status = f.install(f.release("v0.2.0"))
            self.assertEqual((status["state"], status["error"]), ("failed", "other-app"))
            self.assertEqual((f.card / "autorun/autorun.sh").read_text(), "#!/bin/sh\n# boot menu\n")
            self.assertFalse((f.card / "autorun/melodee").exists())

    def test_failed_self_test_does_not_write_card(self):
        with tempfile.TemporaryDirectory() as temp:
            f = Fixture(temp)
            f.installed()
            (Path(temp) / "self-test-fails").write_text("")
            result, status = f.install(f.release("v0.2.0"))
            self.assertEqual((status["state"], status["error"]), ("failed", "self-test"))
            self.assertEqual((f.card / "autorun/melodee/VERSION").read_text(), "v0.1.0 old\n")
            self.assertFalse((f.card / "autorun/melodee.next").exists())
            self.assertEqual(f.call_lines(), [])

    def test_app_that_does_not_start_is_rolled_back(self):
        with tempfile.TemporaryDirectory() as temp:
            f = Fixture(temp)
            f.installed()
            (Path(temp) / "never-ready").write_text("")
            zip_path = f.release("v0.2.0")
            result, status = f.install(zip_path)
            self.assertEqual((status["state"], status["error"], status["version"]), ("failed", "no-start", "v0.2.0"))
            self.assertEqual((f.card / "autorun/melodee/VERSION").read_text(), "v0.1.0 old\n")
            self.assertEqual((f.card / "autorun/autorun.json").read_text(), "{old}\n")
            self.assertEqual((f.card / "autorun/autorun.check").read_text(), "old check\n")
            self.assertFalse((f.card / "autorun/melodee.prev").exists())
            self.assertTrue(zip_path.exists())
            # Previous app restarted from its RAM release; new release removed.
            unit = (f.run_dir / "systemd/system/sample-base.service").read_text()
            self.assertIn(f"ExecStart={f.old}/sample-base --app melodee\n", unit)
            self.assertIn(f"ExecStopPost=/bin/sh {f.old}/restore-stock.sh\n", unit)
            self.assertEqual(os.readlink(f.run_dir / "sample-base"), str(f.old))
            self.assertFalse((f.run_dir / "sample-base-release-sdnew").exists())
            self.assertEqual(sorted(f.run_dir.glob("sample-base-release-*")), [f.old])  # only the running release
            self.assertEqual(f.call_lines()[-1], "start --no-block sample-base.service")
            self.assertNotIn("start ac50.service", f.call_lines())


class SdUpdateAppSideTests(unittest.TestCase):
    def test_versions_zip_list_find_and_status(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "sd-update"
            compile_fixture(binary, "tests/sd_update.cpp", "src/drivers/sd_update.cpp")
            subprocess.run([str(binary), str(Path(temp) / "fixture")], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
