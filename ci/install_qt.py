#!/usr/bin/env python3
"""Installs Qt for the current platform from download.qt.io, without the Qt installer.

Downloads the archives listed in Qt's repository index (Updates.xml) for the modules the app needs,
checks each one against its SHA-1 and extracts it. (aqtinstall can't be used: it doesn't understand the
repository layout that Qt uses since 6.11 on Windows.)

Usage:   python ci/install_qt.py <version> <destination>
Prints the Qt prefix to pass as CMAKE_PREFIX_PATH, e.g. C:/Qt/6.11.3/msvc2022_64
On ARM64 (Windows on ARM, Linux aarch64) it installs Qt's native ARM64 build.
"""
import hashlib
import platform
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

REPOSITORY = "https://download.qt.io/online/qtsdkrepository"
MODULES = {"qtbase", "qtsvg", "qttools", "qttranslations", "qtwayland", "icu"}


def repository(version):
    """(repository folder, package name, folder name of the installed Qt) for this platform and processor."""
    v = version.replace(".", "")   # 6.11.3 -> 6113
    arm = platform.machine().lower() in ("arm64", "aarch64")
    if sys.platform == "win32" and arm:   # native ARM64 build (Windows on ARM)
        return f"{REPOSITORY}/windows_arm64/desktop/qt6_{v}/qt6_{v}", f"qt.qt6.{v}.win64_msvc2022_arm64", "msvc2022_arm64"
    if sys.platform == "win32":
        return f"{REPOSITORY}/windows_x86/desktop/qt6_{v}/qt6_{v}_msvc2022_64", f"qt.qt6.{v}.win64_msvc2022_64", "msvc2022_64"
    if sys.platform == "darwin":
        return f"{REPOSITORY}/mac_x64/desktop/qt6_{v}/qt6_{v}", f"qt.qt6.{v}.clang_64", "macos"
    if arm:   # built on Ubuntu 24.04: needs glibc 2.39 or later
        return f"{REPOSITORY}/linux_arm64/desktop/qt6_{v}/qt6_{v}", f"qt.qt6.{v}.linux_gcc_arm64", "gcc_arm64"
    return f"{REPOSITORY}/linux_x64/desktop/qt6_{v}/qt6_{v}", f"qt.qt6.{v}.linux_gcc_64", "gcc_64"


def fetch(url, attempts=4):
    """Downloads url, trying again a few times if the server doesn't answer (a missing file fails at once)."""
    for attempt in range(1, attempts + 1):
        try:
            with urllib.request.urlopen(url, timeout=300) as response:
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


def extract(archive, target):
    seven_zip = shutil.which("7z") or shutil.which("7zz")
    if seven_zip:   # keeps the symbolic links of the macOS frameworks
        subprocess.run([seven_zip, "x", "-y", "-bso0", "-bsp0", f"-o{target}", str(archive)], check=True)
    else:
        import py7zr
        with py7zr.SevenZipFile(archive) as z:
            z.extractall(target)


def merge(source, target):
    for item in source.iterdir():
        destination = target / item.name
        if item.is_dir() and not item.is_symlink():
            shutil.copytree(item, destination, symlinks=True, dirs_exist_ok=True)
        else:
            if destination.is_symlink() or destination.exists():
                destination.unlink()
            shutil.copy2(item, destination, follow_symlinks=False)


def main():
    version, destination = sys.argv[1], Path(sys.argv[2])
    repo, package_name, folder = repository(version)
    index = ET.fromstring(fetch(f"{repo}/Updates.xml"))
    package = next(p for p in index.iter("PackageUpdate") if p.findtext("Name") == package_name)
    package_version = package.findtext("Version")
    archives = [a.strip() for a in package.findtext("DownloadableArchives").split(",") if a.strip()]
    prefix = destination / version / folder
    prefix.mkdir(parents=True, exist_ok=True)
    for archive in archives:
        if archive.split("-")[0] not in MODULES:
            continue
        url = f"{repo}/{package_name}/{package_version}{archive}"
        data = fetch(url)
        expected = fetch(url + ".sha1").decode().split()[0].lower()
        if hashlib.sha1(data).hexdigest() != expected:
            sys.exit(f"{archive}: checksum mismatch")
        with tempfile.TemporaryDirectory() as tmp:
            file = Path(tmp) / archive
            file.write_bytes(data)
            out = Path(tmp) / "out"
            extract(file, out)
            nested = out / version / folder   # older repository layouts keep <version>/<folder>/ inside
            source = nested if nested.is_dir() else out
            # the ICU libraries (Linux) come without their folder: Qt's installer puts them in lib/
            target = prefix / "lib" if archive.startswith("icu") and not (source / "lib").is_dir() else prefix
            target.mkdir(exist_ok=True)
            merge(source, target)
        print(f"{archive}: {len(data) // 1024 // 1024} MB, SHA-1 ok", file=sys.stderr)
    print(prefix.as_posix())


if __name__ == "__main__":
    main()
