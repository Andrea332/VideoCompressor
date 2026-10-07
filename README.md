# Video Compressor

A small desktop app to compress videos at a chosen quality, with a
**preview of the final file size** before you start compressing.

Handy for staying under the size limits of email, chat apps or upload
platforms: set the limit in MB and the app finds the best quality that fits.

## Features

- **Reliable output size estimate**: instead of a theoretical calculation, the
  app actually encodes a few short samples of the video with the chosen
  settings. This makes the estimate work well with any kind of content (screen
  recordings, camera footage, animations…). Videos up to 20 seconds are
  encoded whole, so the estimate is exact.
- **Fit quality to limit**: enter a maximum size in MB and the app
  automatically searches for the highest quality that stays under it.
- **Simple settings**: quality (CRF), resolution, frame rate, audio bitrate
  (or no audio) and encoding speed. Only sensible options are offered for the
  loaded video (no upscaling, no frame rate higher than the original), and
  vertical videos are handled correctly.
- Drag and drop a video onto the window, progress bar, cancel button and a
  button to open the folder of the created file.
- Output as **MP4 (H.264 + AAC)**, playable almost everywhere.
- **FFmpeg included**: the packaged app needs nothing else installed.

## Using the app

Windows only (developed and tested on Windows 11).

1. Extract `video_compressor.zip`.
2. Run `video_compressor.exe` inside the `video_compressor` folder.

Keep the folder as it is: the program needs the `_internal` folder next to it.

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
2. The bytes produced give the average video bitrate, to which the chosen
   audio bitrate and about 1% for the MP4 container are added.
3. The estimate updates automatically whenever a setting changes; results that
   were already computed are reused.

For videos longer than 20 seconds the margin of error is roughly ±10%. That
is why *Fit quality to limit* aims for 94% of the limit you set.

## License

The source code of this app is released under the MIT License. See
[LICENSE](LICENSE).

The packaged app includes [FFmpeg](https://ffmpeg.org/) (gyan.dev build),
which is licensed under the GPLv3. Its license and build information, including
a link to the exact FFmpeg source code, are in `_internal/ffmpeg/` inside the
app folder.
