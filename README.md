# Video Compressor

A small desktop app to compress videos at a chosen quality, with a
**preview of the final file size** before you start compressing.

Handy for staying under the size limits of email, chat apps or upload
platforms: set the limit in MB and the app finds the best quality that fits.

## Download

**[Download the latest version](https://github.com/Andrea332/VideoCompressor/releases/latest)**
(Windows 10/11, 64-bit; developed and tested on Windows 11). Previous versions
and the changes in each one are on the
[releases page](https://github.com/Andrea332/VideoCompressor/releases).

1. Download `VideoCompressor-<version>-win64.zip` from the release.
2. Extract it.
3. Run `video_compressor.exe` inside the `video_compressor` folder.

FFmpeg is included: there is nothing else to install. Keep the folder as it
is, because the program needs the `_internal` folder next to it.

The app is not code-signed, so Windows SmartScreen may show "Windows protected
your PC" the first time: click **More info** → **Run anyway**.

## Features

- **Reliable output size estimate**: instead of a theoretical calculation, the
  app actually encodes a few short samples of the video with the chosen
  settings. This makes the estimate work well with any kind of content (screen
  recordings, camera footage, animations…). Videos up to 20 seconds are
  encoded whole, so the estimate is exact.
- **Fit quality to limit**: enter a maximum size in MB and the app
  automatically searches for the highest quality that stays under it.
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
- **Hardware acceleration**: codecs that a GPU in your PC can encode are marked
  with ⚡ (NVIDIA NVENC, AMD AMF and Intel Quick Sync, for H.264, H.265 and
  AV1). Acceleration can be:
  - **Automatic**: the best GPU that can encode the chosen codec (NVIDIA, then
    Intel, then AMD), otherwise the CPU;
  - **Manual**: you choose the GPU;
  - **Off**: always the CPU.

  The window always states which codec will be used, whether hardware
  acceleration is on and which device (GPU or CPU, by name) does the work.
  GPU encoding is much faster, but files are a bit larger at the same quality.
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
- Drag and drop a video onto the window, progress bar, cancel button and a
  button to open the folder of the created file.
- **FFmpeg included**: the packaged app needs nothing else installed.

## Running from source

Requirements:

- Python 3.9 or later
- the dependencies in `requirements.txt` (PySide6)
- [FFmpeg](https://ffmpeg.org/) (`ffmpeg.exe` and `ffprobe.exe`), either in an
  `ffmpeg/` folder inside the project or in your `PATH`:

  ```bash
  winget install Gyan.FFmpeg
  ```

```bash
pip install -r requirements.txt
python video_compressor.py
```

You can also pass a video as an argument to load it right away:

```bash
python video_compressor.py "C:\path\to\video.mp4"
```

## Building the app

The app is packaged with [PyInstaller](https://pyinstaller.org/) using the
included `.spec` file:

```bash
pip install pyinstaller
python -m PyInstaller video_compressor.spec
```

This creates:

- `dist/video_compressor/`: the app folder, with FFmpeg inside
  (about 540 MB)
- `dist/video_compressor.zip`: the same folder zipped, ready to share
  (about 215 MB)

The FFmpeg to bundle is taken from the `ffmpeg/` folder of the project
(`ffmpeg.exe`, `ffprobe.exe` and, if present, `LICENSE` and `README.txt`). If
that folder doesn't exist, the FFmpeg found in `PATH` is used. The
[gyan.dev](https://www.gyan.dev/ffmpeg/builds/) *full* build is recommended:
it is what the app is tested with and includes all FFmpeg features.

The app is built as a folder rather than a single `.exe` on purpose: a
single-file build would unpack FFmpeg (over 500 MB) to the temp folder at every
launch, making startup several times slower.

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

With slow encoders the estimate takes longer, because the samples are really
encoded: H.266/VVC can take a minute or more, while GPU encoders take a few
seconds.

## License

The source code of this app is released under the MIT License. See
[LICENSE](LICENSE).

The packaged app includes [FFmpeg](https://ffmpeg.org/) (gyan.dev build),
which is licensed under the GPLv3. Its license and build information, including
a link to the exact FFmpeg source code, are in `_internal/ffmpeg/` inside the
app folder.
