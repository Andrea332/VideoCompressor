#!/usr/bin/env python3
"""Downloads the FFmpeg build bundled with the app into the given folder (normally ffmpeg/), checking SHA-256.

  Windows x64:    gyan.dev "full-shared" build: ffmpeg.exe, ffprobe.exe and their DLLs
  Windows ARM64:  BtbN "gpl-shared" build for ARM64: ffmpeg.exe, ffprobe.exe and their DLLs
  macOS:          martin-riedl.de static build for Apple Silicon: ffmpeg, ffprobe
  Linux:          BtbN "gpl-shared" build for x86_64 or ARM64 (aarch64): bin/ffmpeg, bin/ffprobe and lib/

Every build is GPLv3: its license and a note on where its source code is go in the folder too.

Usage:   python ci/get_ffmpeg.py <folder>
"""
import hashlib
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GPL = ROOT / "licenses" / "Qt-GPL-3.0-only.txt"   # the GPLv3 text, the same for every program

WINDOWS = ("https://www.gyan.dev/ffmpeg/builds/packages/ffmpeg-9.0.2-full_build-shared.7z",
           "4d2060a8b34a940aa47d785142055bb92a63053781e55f2ace4546edd519a8f5")
MACOS = "https://ffmpeg.martin-riedl.de/download/macos/arm64/1789931890_9.0.2"
BTBN = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest"   # rolling: latest 9.0.x
ARM = platform.machine().lower() in ("arm64", "aarch64")


def fetch(url, attempts=4):
    """Downloads url, trying again a few times if the server doesn't answer (a missing file fails at once)."""
    for attempt in range(1, attempts + 1):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "VideoCompressor-CI"}), timeout=600) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            if error.code < 500 or attempt == attempts:
                raise
            print(f"{url}: {error}, trying again", file=sys.stderr)
        except OSError as error:   # timeouts, refused or reset connections
            if attempt == attempts:
                raise
            print(f"{url}: {error}, trying again", file=sys.stderr)
        time.sleep(15 * attempt)


def checked(data, sha256, name):
    actual = hashlib.sha256(data).hexdigest()
    if actual != sha256.lower():
        sys.exit(f"{name}: SHA-256 mismatch (expected {sha256}, got {actual})")
    print(f"{name}: {len(data) // 1024 // 1024} MB, SHA-256 ok", file=sys.stderr)
    return data


def btbn(target, tmp):
    """Downloads and extracts BtbN's GPL shared build for target (e.g. "linux64"), returns its folder."""
    name = f"ffmpeg-n9.0-latest-{target}-gpl-shared-9.0"
    archive_name = name + (".zip" if target.startswith("win") else ".tar.xz")
    checksums = fetch(f"{BTBN}/checksums.sha256").decode()
    sha256 = next(line.split()[0] for line in checksums.splitlines() if line.rstrip().endswith(archive_name))
    archive = tmp / archive_name
    archive.write_bytes(checked(fetch(f"{BTBN}/{archive_name}"), sha256, archive_name))
    if archive_name.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            z.extractall(tmp)
    else:
        with tarfile.open(archive) as tar:
            tar.extractall(tmp, filter="tar")
    return next(tmp.glob(f"ffmpeg-n9.0-*-{target}-gpl-shared-*/"))


def btbn_readme(out, target, system):
    (out / "README.txt").write_text(
        f"FFmpeg 9.0, GPL shared build for {system} by BtbN: https://github.com/BtbN/FFmpeg-Builds "
        f"(ffmpeg-n9.0-latest-{target}-gpl-shared-9.0)\n"
        "Licensed under the GNU GPL version 3 (LICENSE.txt).\n"
        "FFmpeg source code: https://github.com/FFmpeg/FFmpeg/tree/release/9.0\n"
        "Build scripts, with the version and source of every library: https://github.com/BtbN/FFmpeg-Builds\n")


def windows(out, tmp):
    if ARM:   # gyan.dev has no ARM64 build
        root = btbn("winarm64", tmp)
        for file in [root / "bin" / "ffmpeg.exe", root / "bin" / "ffprobe.exe", *(root / "bin").glob("*.dll"),
                     root / "LICENSE.txt"]:
            shutil.copy2(file, out)
        btbn_readme(out, "winarm64", "Windows on ARM (ARM64)")
        return
    url, sha256 = WINDOWS
    archive = tmp / "ffmpeg.7z"
    archive.write_bytes(checked(fetch(url), sha256, url))
    seven_zip = shutil.which("7z") or "C:/Program Files/7-Zip/7z.exe"
    subprocess.run([seven_zip, "x", "-y", "-bso0", "-bsp0", f"-o{tmp}", str(archive)], check=True)
    root = next(tmp.glob("ffmpeg-*-shared"))
    for file in [root / "bin" / "ffmpeg.exe", root / "bin" / "ffprobe.exe", *(root / "bin").glob("*.dll"),
                 root / "LICENSE", root / "README.txt"]:
        shutil.copy2(file, out)


def macos(out, tmp):
    for tool in ("ffmpeg", "ffprobe"):
        url = f"{MACOS}/{tool}.zip"
        sha256 = fetch(url + ".sha256").decode().split()[0]
        archive = tmp / f"{tool}.zip"
        archive.write_bytes(checked(fetch(url), sha256, url))
        with zipfile.ZipFile(archive) as z:
            z.extract(tool, out)
        (out / tool).chmod(0o755)
    shutil.copy2(GPL, out / "LICENSE.txt")
    (out / "README.txt").write_text(
        "FFmpeg 9.0.2, static build for macOS on Apple Silicon by Martin Riedl: https://ffmpeg.martin-riedl.de\n"
        "Licensed under the GNU GPL version 3 (LICENSE.txt).\n"
        "FFmpeg source code: https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz\n"
        "Build scripts, with the version and source of every library: https://gitlab.com/martinr92/ffmpeg\n")


def linux(out, tmp):
    target = "linuxarm64" if ARM else "linux64"
    root = btbn(target, tmp)
    (out / "bin").mkdir()
    for tool in ("ffmpeg", "ffprobe"):
        shutil.copy2(root / "bin" / tool, out / "bin")
    shutil.copytree(root / "lib", out / "lib", symlinks=True, ignore=shutil.ignore_patterns("pkgconfig", "*.a"))
    shutil.copy2(root / "LICENSE.txt", out / "LICENSE.txt")
    btbn_readme(out, target, "Linux ARM64 (aarch64)" if ARM else "Linux x86_64")


def main():
    out = Path(sys.argv[1]).resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    with tempfile.TemporaryDirectory() as tmp:
        {"win32": windows, "darwin": macos}.get(sys.platform, linux)(out, Path(tmp))
    for file in sorted(out.rglob("*")):
        if file.is_file() and not file.is_symlink():
            print(f"  {file.relative_to(out)}  {file.stat().st_size // 1024} KB", file=sys.stderr)


if __name__ == "__main__":
    main()
