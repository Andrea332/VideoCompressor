#!/usr/bin/env python3
"""Downloads the FFmpeg build bundled with the app into the given folder (normally ffmpeg/), checking SHA-256.

  Windows:  gyan.dev "full-shared" build: ffmpeg.exe, ffprobe.exe and their DLLs
  macOS:    martin-riedl.de static build for Apple Silicon: ffmpeg, ffprobe
  Linux:    BtbN "gpl-shared" build for x86_64: bin/ffmpeg, bin/ffprobe and lib/

Every build is GPLv3: its license and a note on where its source code is go in the folder too.

Usage:   python ci/get_ffmpeg.py <folder>
"""
import hashlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GPL = ROOT / "licenses" / "Qt-GPL-3.0-only.txt"   # the GPLv3 text, the same for every program

WINDOWS = ("https://www.gyan.dev/ffmpeg/builds/packages/ffmpeg-9.0.2-full_build-shared.7z",
           "4d2060a8b34a940aa47d785142055bb92a63053781e55f2ace4546edd519a8f5")
MACOS = "https://ffmpeg.martin-riedl.de/download/macos/arm64/1789931890_9.0.2"
LINUX = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest"
LINUX_ARCHIVE = "ffmpeg-n9.0-latest-linux64-gpl-shared-9.0.tar.xz"


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "VideoCompressor-CI"})
    with urllib.request.urlopen(request, timeout=600) as response:
        return response.read()


def checked(data, sha256, name):
    actual = hashlib.sha256(data).hexdigest()
    if actual != sha256.lower():
        sys.exit(f"{name}: SHA-256 mismatch (expected {sha256}, got {actual})")
    print(f"{name}: {len(data) // 1024 // 1024} MB, SHA-256 ok", file=sys.stderr)
    return data


def windows(out, tmp):
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
    checksums = fetch(f"{LINUX}/checksums.sha256").decode()
    sha256 = next(line.split()[0] for line in checksums.splitlines() if line.rstrip().endswith(LINUX_ARCHIVE))
    archive = tmp / LINUX_ARCHIVE
    archive.write_bytes(checked(fetch(f"{LINUX}/{LINUX_ARCHIVE}"), sha256, LINUX_ARCHIVE))
    with tarfile.open(archive) as tar:
        tar.extractall(tmp, filter="tar")
    root = next(tmp.glob("ffmpeg-n9.0-*-linux64-gpl-shared-*"))
    (out / "bin").mkdir()
    for tool in ("ffmpeg", "ffprobe"):
        shutil.copy2(root / "bin" / tool, out / "bin")
    shutil.copytree(root / "lib", out / "lib", symlinks=True, ignore=shutil.ignore_patterns("pkgconfig", "*.a"))
    shutil.copy2(root / "LICENSE.txt", out / "LICENSE.txt")
    (out / "README.txt").write_text(
        f"FFmpeg 9.0, GPL shared build for Linux x86_64 by BtbN: https://github.com/BtbN/FFmpeg-Builds ({LINUX_ARCHIVE})\n"
        "Licensed under the GNU GPL version 3 (LICENSE.txt).\n"
        "FFmpeg source code: https://github.com/FFmpeg/FFmpeg/tree/release/9.0\n"
        "Build scripts, with the version and source of every library: https://github.com/BtbN/FFmpeg-Builds\n")


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
