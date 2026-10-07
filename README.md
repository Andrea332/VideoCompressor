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

## Requirements

- Windows (developed and tested on Windows 11)
- [ffmpeg](https://ffmpeg.org/) and ffprobe in your `PATH`:

  ```bash
  winget install Gyan.FFmpeg
  ```

  After installing, open a new terminal (or restart the app) so the updated
  `PATH` is picked up.

To run it from source you also need:

- Python 3.9 or later
- the dependencies in `requirements.txt` (PySide6)

## Running from source

```bash
pip install -r requirements.txt
python video_compressor.py
```

You can also pass a video as an argument to load it right away:

```bash
python video_compressor.py "C:\path\to\video.mp4"
```

## Building the executable

The Windows executable is built with [PyInstaller](https://pyinstaller.org/)
using the included `.spec` file:

```bash
pip install pyinstaller
python -m PyInstaller video_compressor.spec
```

The file is created at `dist/video_compressor.exe`. The executable **does not
include ffmpeg**: ffmpeg and ffprobe must be installed and in the `PATH` on the
PC where you run it.

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

Released under the MIT License. See [LICENSE](LICENSE).
