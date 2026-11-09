#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Fetch checksum-verified public platform references into ignored local storage.

This is a documentation/research helper, not a build dependency or installer.
Only selected source files are extracted; nothing is sent to a device.
"""
import hashlib
from pathlib import Path
import shutil
import tarfile
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
REVISION = "d09cedcdce9d48edda975be31d0e4b88ba67eb25"
PACKAGES = {
    "linux-imb-6.12.60+git-r0": "9d6999d9f69f5afd396a634facadc9219dcb85407264273d912355dd6db38f9b",
    "edisksd-0.19-r0": "ece9cd1f6e04333dd016499e9f7bf0b65e878af45f3d2a463959212468acc80f",
}
LINUX_FILES = (
    "arch/arm64/boot/dts/rockchip/rk3566-az07-ac50.dts",
    "arch/arm64/boot/dts/rockchip/rk3566-az07.dtsi",
    "arch/arm64/configs/az07_defconfig",
    "sound/drivers/serial-generic.c", "sound/soc/codecs/ak4558.c",
    "sound/soc/codecs/simple-amplifier.c", "sound/soc/codecs/simple-mute.c",
    "sound/soc/generic/simple-card-utils.c", "drivers/gpu/drm/tiny/st7796s.c",
    "drivers/power/supply/generic-adc-mains.c", "drivers/input/misc/rk805-pwrkey.c",
    "drivers/usb/gadget/function/f_uac2.c", "drivers/usb/gadget/function/u_audio.c",
    "drivers/usb/gadget/function/u_audio.h", "drivers/usb/gadget/function/u_uac2.h",
    "drivers/usb/gadget/function/uac_common.h",
)


def main():
    cache = ROOT / ".local/hardware-reference"
    cache.mkdir(parents=True, exist_ok=True)
    for package, digest in PACKAGES.items():
        name = package + "-patched.tar.xz"
        archive = cache / name
        if not archive.exists():
            path = "aarch64-poky-linux/" + package + "/" + name
            url = "https://media.githubusercontent.com/media/inmusic-software/az0x-open-source/" + REVISION + "/" + urllib.parse.quote(path)
            temporary = archive.with_suffix(".part")
            with urllib.request.urlopen(url, timeout=60) as response, temporary.open("wb") as output:
                shutil.copyfileobj(response, output)
            if hashlib.sha256(temporary.read_bytes()).hexdigest() != digest:
                temporary.unlink()
                raise ValueError("Public source archive checksum mismatch: " + name)
            temporary.replace(archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
            raise ValueError("Cached source archive checksum mismatch: " + name)
        destination = cache / name.removesuffix(".tar.xz")
        with tarfile.open(archive, mode="r|xz") as source:
            for member in source:
                relative = Path(member.name)
                if not member.isfile() or relative.is_absolute() or ".." in relative.parts:
                    continue
                if package.startswith("linux-") and member.name not in {"kernel-source/" + item for item in LINUX_FILES}:
                    continue
                output = destination / relative
                output.parent.mkdir(parents=True, exist_ok=True)
                with source.extractfile(member) as data, output.open("wb") as target:
                    shutil.copyfileobj(data, target)
        print("Verified public reference: " + package)


if __name__ == "__main__":
    main()
