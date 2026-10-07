# -*- mode: python ; coding: utf-8 -*-
import os
import shutil


def ffmpeg_datas():
    """FFmpeg to bundle: from the ffmpeg/ folder if present, otherwise the one in PATH."""
    bin_dir = doc_dir = os.path.join(SPECPATH, 'ffmpeg')
    if not os.path.isfile(os.path.join(bin_dir, 'ffmpeg.exe')):
        found = shutil.which('ffmpeg')
        if not found:
            raise SystemExit('FFmpeg not found: put ffmpeg.exe and ffprobe.exe in ffmpeg/ or add them to PATH')
        bin_dir = os.path.dirname(os.path.realpath(found))   # follows the winget symlink
        doc_dir = os.path.dirname(bin_dir)                    # gyan.dev layout: <root>/bin/ffmpeg.exe
    tools = [os.path.join(bin_dir, n) for n in ('ffmpeg.exe', 'ffprobe.exe')]
    for tool in tools:
        if not os.path.isfile(tool):
            raise SystemExit(f'Missing {tool}')
    # the FFmpeg license (GPLv3) and build info must ship with the binaries
    docs = [os.path.join(doc_dir, n) for n in ('LICENSE', 'README.txt')]
    print(f'Bundling FFmpeg from {bin_dir}')
    return [(f, 'ffmpeg') for f in tools + [d for d in docs if os.path.isfile(d)]]


a = Analysis(
    ['video_compressor.py'],
    pathex=[],
    binaries=[],
    datas=ffmpeg_datas(),
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

# One-folder build: with FFmpeg inside, a one-file exe would unpack ~540 MB
# to the temp folder at every launch (slow start, leftovers after a crash).
exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='video_compressor',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=True,
    upx_exclude=[],
    name='video_compressor',
)

# zip of the folder, ready to share
print('Creating video_compressor.zip...')
shutil.make_archive(os.path.join(DISTPATH, 'video_compressor'), 'zip', DISTPATH, 'video_compressor')
