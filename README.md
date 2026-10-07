# Video Compressor

A small desktop app to compress videos at a chosen quality, with a
**preview of the final file size** before you start compressing.

Handy for staying under the size limits of email, chat apps or upload
platforms: set the limit in MB and the app finds the best quality that fits.

<p align="center">
  <!-- taken automatically for every release (see .github/workflows/build.yml): this is always the latest one -->
  <img src="https://github.com/Andrea332/VideoCompressor/releases/latest/download/VideoCompressor-screenshot.png"
       alt="Video Compressor with a video loaded: preview frame, settings and estimated output size" width="640">
</p>

## Download

**[Download the latest version](https://github.com/Andrea332/VideoCompressor/releases/latest)**
for Windows (x64 or ARM64), macOS (Apple Silicon) or Linux (x86_64 or ARM64).
Previous versions and the changes in each one are on the
[releases page](https://github.com/Andrea332/VideoCompressor/releases).
FFmpeg is included in every download: there is nothing else to install.

Every system also has a **portable** version (the files ending in
`-portable`): extract it and run it from wherever you like, even a USB stick,
without installing anything.

### Windows 10/11

There are two versions: `win64` for most PCs (Intel and AMD processors) and
`win-arm64` for Windows 11 PCs with an ARM processor, such as Snapdragon X
laptops. If you are not sure, open **Settings → System → About**: *System type*
says "x64-based processor" or "ARM-based processor".

- **Installer** (`VideoCompressor-<version>-win64.exe` or
  `-win-arm64.exe`, the smaller download):
  run it and follow the steps. By default it installs for your user only, with
  no administrator rights; you can choose to install for all users instead.
  It adds Video Compressor to the Start menu (and, if you want, to the
  desktop), and you can uninstall it from Windows Settings like any other app.
  The app updates itself from then on (see *Updates* below).
- **Portable** (`VideoCompressor-<version>-win64-portable.zip` or
  `-win-arm64-portable.zip`): extract it and run `VideoCompressor.exe` inside
  the extracted folder. Keep the folder as it is, because the program needs the
  files next to it (Qt libraries and the `ffmpeg` folder). It keeps its
  settings in `VideoCompressor.ini` in the same folder and writes nothing to
  the registry, so it leaves no trace on the PC (delete that file to keep the
  settings in the registry instead).

The app is not code-signed, so Windows SmartScreen may show "Windows protected
your PC" the first time: click **More info** → **Run anyway**.

### macOS 13 or later, Apple Silicon (M1 or later)

Open `VideoCompressor-<version>-macos-arm64.dmg` and drag Video Compressor into
Applications. The portable version, `VideoCompressor-<version>-macos-arm64-portable.zip`,
is the same app in a zip: extract it and run it from any folder.

The app is not signed by a registered Apple developer, so the first time macOS
blocks it. Open **System Settings → Privacy & Security**, scroll down and click
**Open Anyway** next to the message about Video Compressor, then confirm. This
is needed only once.

### Linux (x86_64 or ARM64)

Download `VideoCompressor-<version>-linux-x86_64.AppImage` (most PCs) or
`-linux-aarch64.AppImage` (ARM64, for example a Raspberry Pi 4 or 5 with a
64-bit system), make it executable and run it:

```bash
chmod +x VideoCompressor-*.AppImage
./VideoCompressor-*.AppImage
```

The x86_64 version works on distributions from 2022 on (for example Ubuntu
22.04, Debian 12, Fedora 36 and later), the ARM64 one on distributions from 2024
on (Ubuntu 24.04, Debian 13, Fedora 40, Raspberry Pi OS based on Debian 13 and
later). If it doesn't start because FUSE is missing, run it with
`--appimage-extract-and-run`, or use the portable version.

The portable version, `VideoCompressor-<version>-linux-x86_64-portable.tar.gz`
(or `-linux-aarch64-portable.tar.gz`), is the same app as a folder, which needs
no FUSE: extract it anywhere and run `VideoCompressor` inside it.

```bash
tar -xzf VideoCompressor-*-portable.tar.gz
./VideoCompressor-*-portable/VideoCompressor
```

It keeps its settings in `VideoCompressor.ini` in that folder instead of in
`~/.config`.

## Features

- **Reliable output size estimate**: instead of a theoretical calculation, the
  app actually encodes a few short samples of the video with the chosen
  settings. This makes the estimate work well with any kind of content (screen
  recordings, camera footage, animations…). Videos up to 20 seconds are
  encoded whole, so the estimate is exact.
- **Fit quality to limit**: enter a maximum size in MB and the app
  automatically searches for the highest quality that stays under it.
- **Constant quality or a bitrate of your choice**: by default the encoder
  keeps the chosen quality and spends the bits each scene needs, which gives the
  best quality for the size. You can instead set the video bitrate in kbps,
  where the encoder has these modes:
  - **Variable bitrate (VBR)**: the average you set, more for complex scenes
    and less for simple ones (up to twice the average). Every encoder has it.
  - **Constant bitrate (CBR)**: the same bitrate all the time, for streaming or
    for devices that need it. x264, x265, VP9 and the GPU encoders have it;
    SVT-AV1, VVenC and Xvid don't. GPU modes are offered only if the GPU accepts
    them.

  With a bitrate, the size is simply bitrate × duration, so it is shown right
  away, and *Fit bitrate to limit* computes the bitrate that fits.
- **Many output formats and codecs**:

  | Format | Video codecs | Audio codecs |
  |--------|--------------|--------------|
  | MP4    | H.264, H.265/HEVC, AV1, VP9, H.266/VVC, MPEG-4 (Xvid) | AAC, Opus, MP3 |
  | MKV    | H.264, H.265/HEVC, AV1, VP9, H.266/VVC, MPEG-4 (Xvid) | AAC, Opus, MP3, Vorbis |
  | WebM   | AV1, VP9 | Opus, Vorbis |
  | MOV    | H.264, H.265/HEVC | AAC, MP3 |
  | AVI    | MPEG-4 (Xvid) | MP3 |

  Only combinations that work together are offered. The default, MP4 with
  H.264 and AAC, plays almost everywhere.
- **Hardware acceleration**: codecs that a GPU in your computer can encode are
  marked with ⚡: NVIDIA NVENC (Windows, Linux), AMD AMF (Windows), Intel Quick
  Sync (Windows, Linux) for H.264, H.265 and AV1, and Apple VideoToolbox (macOS)
  for H.264 and H.265. Acceleration can be:
  - **Automatic**: the best GPU that can encode the chosen codec (NVIDIA, then
    Apple, Intel and AMD), otherwise the CPU;
  - **Manual**: you choose the GPU;
  - **Off**: always the CPU.

  The window always states which codec will be used, whether hardware
  acceleration is on and which device (GPU or CPU, by name) does the work.
  GPU encoding is much faster, but files are a bit larger at the same quality.
  The ARM64 versions for Windows and Linux encode on the CPU.
- **Any input video**: anything FFmpeg can read, including phone videos
  recorded in portrait, files with cover art and files with subtitles. The
  main video and the first audio track are kept; subtitles are not copied.
- **Simple settings**: quality, resolution, frame rate, audio codec and
  bitrate (or no audio) and encoding speed. Only sensible options are offered
  for the loaded video (no upscaling, no frame rate higher than the original),
  and vertical videos are handled correctly.
- **Smooth seeking**: every encoder places keyframes at the same, chosen
  interval (every 10 s by default for the smallest file, or 5, 2 or 1 s for
  faster and more precise seeking when editing or streaming). Keyframes are
  closed-GOP (IDR) points, and the seek index is written at the start of the
  file (MP4, MOV, MKV, WebM), so players can jump anywhere right away, even
  over the network. Shorter intervals make the file bigger, especially for
  videos with little motion such as screen recordings.
- **Preview of the source video**: as soon as the path points to a video, a
  frame of it (at 10% of its length) appears next to it.
- **Hard to miss when it's done**: at the end a green box shows the new file,
  its size and how much smaller it is than the original, with buttons to play
  it or open its folder (orange if it came out over the limit, red with the
  reason if FFmpeg failed). If the window is in the background, its taskbar
  button flashes (on macOS the Dock icon bounces). While the size is being
  estimated, a wheel turns next to it.
- Drag and drop a video onto the window, progress bar and cancel button.
- **FFmpeg included**: the packaged app needs nothing else installed.

## Updates

A few seconds after starting, at most once a day, the app asks GitHub which is
the latest release; nothing about you or your videos is sent. If there is a
newer version, a notice appears at the top of the window:

- **Update now** (Windows, installed with the installer): downloads the new
  installer, checks it against the SHA-256 published by GitHub, installs it
  and restarts the app.
- **Download** (portable versions, macOS, Linux): opens the release page.

Untick **Check for updates automatically** at the bottom of the window to turn
it off; **Check now** checks right away.

## Building from source

The app is written in C++20 with [Qt 6](https://www.qt.io/) (Widgets) and
built with CMake. Every push is built, tested and packaged on Windows, Linux
and macOS by [GitHub Actions](.github/workflows/build.yml); for a version tag
the packages are uploaded to the release. The workflow shows the exact steps
for each platform: `ci/install_qt.py` installs Qt, `ci/get_ffmpeg.py`
downloads the FFmpeg that gets bundled, and `packaging/` creates the Linux
AppImage and the macOS disk image. The ARM64 versions are built and tested on
GitHub's ARM64 machines, with Qt's and FFmpeg's native ARM64 builds.

On Windows you need:

- Visual Studio 2022 or later with the *Desktop development with C++* workload
- CMake 3.21 or later and Ninja:

  ```bash
  winget install Kitware.CMake Ninja-build.Ninja
  ```

- Qt 6.5 or later for MSVC 64-bit (developed with Qt 6.11.3). The build script
  looks for it in `C:\Qt\6.11.3\msvc2022_64`; otherwise pass `-QtDir` or set
  `QTDIR`.
- [FFmpeg](https://ffmpeg.org/) (`ffmpeg.exe` and `ffprobe.exe`), either in an
  `ffmpeg/` folder inside the project or in your `PATH`:

  ```bash
  winget install Gyan.FFmpeg
  ```

- To create the installer, [Inno Setup](https://jrsoftware.org/isinfo.php) 6
  (without it, only the zip is created):

  ```bash
  winget install JRSoftware.InnoSetup
  ```

Then, from PowerShell:

```powershell
.\build.ps1              # build: build\VideoCompressor.exe
.\build.ps1 -Test        # build and run the tests
.\build.ps1 -Package     # build and create the installer and the zip in build\
```

The script sets up the Visual Studio compiler environment by itself. To run
`build\VideoCompressor.exe` directly, Qt's `bin` folder must be in `PATH`. You
can pass a video as an argument to load it right away:

```powershell
build\VideoCompressor.exe "C:\path\to\video.mp4"
```

The package contains `VideoCompressor.exe`, only the Qt libraries and plugins
it needs, the Microsoft C++ runtime and FFmpeg. The FFmpeg to bundle is the
`ffmpeg/` folder of the project, copied as it is; `python ci/get_ffmpeg.py
ffmpeg` fills it with the build used for the releases. If that folder doesn't
exist, on Windows the FFmpeg found in `PATH` is used. The same FFmpeg is copied
to `build\ffmpeg`, so the tests use exactly what gets shipped.

The FFmpeg builds bundled with the releases:

| Platform | Build | Contents of `ffmpeg/` |
|----------|-------|-----------------------|
| Windows x64 | [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) *full-shared* | `ffmpeg.exe`, `ffprobe.exe` and their DLLs |
| Windows ARM64 | [BtbN](https://github.com/BtbN/FFmpeg-Builds) *winarm64-gpl-shared* | `ffmpeg.exe`, `ffprobe.exe` and their DLLs |
| macOS | [Martin Riedl](https://ffmpeg.martin-riedl.de/), static, Apple Silicon | `ffmpeg`, `ffprobe` |
| Linux | [BtbN](https://github.com/BtbN/FFmpeg-Builds) *gpl-shared*, x86_64 or ARM64 | `bin/ffmpeg`, `bin/ffprobe`, `lib/` |

On Windows the *full-shared* build has every feature of the *full* build, but
`ffmpeg.exe` and `ffprobe.exe` share one copy of the libraries instead of
containing one each: 230 MB instead of 424 MB.

The app icon is drawn in `resources/icon.svg` (and `icon-small.svg`, a
simplified version for 16–24 px). After editing them, regenerate the icons for
every platform (`icon.ico`, `icon.icns`, `icon.png`), with Qt's `bin` folder in
`PATH`:

```powershell
cmake --build build --target make_icon
build\make_icon.exe resources\icon.svg resources\icon-small.svg resources
```

The tests (`tests/`) generate their own videos with FFmpeg and drive the real
window offscreen: every format, codec and device combination (checking which
encoder actually wrote the file), every bitrate mode of every encoder, portrait
phone videos, cover art, keyframe spacing and closed GOP, the position of the
seek index, the accuracy of the estimate, *Fit quality to limit* and *Fit
bitrate to limit*, and the box shown at the end. GPU encoders are tested only
when the PC has them.

## How the estimate works

1. Four 4-second samples spread across the video are encoded with the chosen
   video settings.
2. Each sample starts with a keyframe, while the real file only has one every
   few seconds: the size of the keyframes the samples have in excess is
   subtracted. Without this, videos with little motion (where keyframes are
   most of the size) would be estimated up to 50% too big.
3. The bytes produced give the average video bitrate, to which the chosen
   audio bitrate and about 1% for the container are added.
4. The estimate updates automatically whenever a setting changes; results that
   were already computed are reused.

For videos longer than 20 seconds the margin of error is roughly ±10%. That
is why *Fit quality to limit* aims for 94% of the limit you set.

With a bitrate (VBR or CBR) nothing needs to be encoded: the size is the video
and audio bitrates × the duration, plus about 1% for the container. Short
samples would be misleading here, because encoders take a few seconds to settle
on a bitrate. Encoders keep to a constant bitrate within a few %; with a
variable one, simple videos (such as screen recordings) come out smaller, and
GPU encoders can end up to about 10% above the average. *Fit bitrate to limit*
aims for 94% of the limit, 90% for GPU encoders with a variable bitrate.

With slow encoders the estimate takes longer, because the samples are really
encoded: H.266/VVC can take a minute or more, while GPU encoders take a few
seconds.

## License

The source code of this app is released under the MIT License. See
[LICENSE](LICENSE).

The packaged app also includes:

- [Qt 6](https://www.qt.io/), used under the LGPLv3. Its license texts are in
  the `licenses` folder of the package; the Qt source code is available at
  [download.qt.io](https://download.qt.io/official_releases/qt/).
- [FFmpeg](https://ffmpeg.org/), licensed under the GPLv3. Its license and
  build information, including where to find the FFmpeg source code, are in the
  `ffmpeg` folder of the package (in the app's `Contents/Resources` on macOS).
