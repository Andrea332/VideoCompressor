#!/usr/bin/env python3
"""
Video Compressor - Qt GUI to compress videos at a chosen quality,
with a preview of the final file size.

The estimate is made by actually encoding a few short samples of the video
with the chosen settings, so it is reliable even with very different kinds of
content (screen recordings, camera footage, animations...).

Requirements:
    - Python 3.9+
    - PySide6         ->  pip install PySide6
    - ffmpeg/ffprobe  ->  bundled in the executable; when running from source
                          they are taken from the ffmpeg/ folder or from PATH
                          (winget install Gyan.FFmpeg)

Run:
    python video_compressor.py
"""

import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from PySide6.QtCore import QObject, QProcess, Qt, QTimer, QUrl, Signal
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import (
    QApplication, QComboBox, QDoubleSpinBox, QFileDialog, QFormLayout, QGroupBox,
    QHBoxLayout, QLabel, QLineEdit, QMessageBox, QPlainTextEdit, QProgressBar,
    QPushButton, QSlider, QVBoxLayout, QWidget,
)

VIDEO_FILTER = "Videos (*.mp4 *.mkv *.mov *.avi *.webm *.m4v *.wmv *.flv);;All files (*)"
NO_WINDOW = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
MB = 1024 * 1024

CRF_MIN, CRF_MAX, CRF_DEFAULT = 18, 45, 28   # lower CRF = higher quality
SAMPLE_COUNT, SAMPLE_LEN = 4, 4.0            # samples used for the estimate
WHOLE_IF_SHORTER = 20.0                      # shorter videos are encoded whole (exact estimate)
CONTAINER_OVERHEAD = 1.01                    # ~1% for the MP4 container
LIMIT_MARGIN = 0.94                          # safety margin for "Fit to limit"

PRESETS = [("Balanced", "medium"), ("Fast", "veryfast"), ("Best quality (slow)", "slow")]
AUDIO = [("96 kbps", 96), ("128 kbps", 128), ("64 kbps", 64), ("No audio", 0)]
RES_STEPS = [1080, 720, 540, 480, 360]
FPS_STEPS = [30, 24, 15]


# ---------------------------------------------------------------- utilities
def find_tool(name):
    """Prefers the FFmpeg bundled with the app (ffmpeg/ folder), otherwise uses PATH."""
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    bundled = os.path.join(base, "ffmpeg", name + (".exe" if sys.platform == "win32" else ""))
    return bundled if os.path.isfile(bundled) else shutil.which(name)


def parse_rate(text):
    try:
        num, den = text.split("/")
        return float(num) / float(den) if float(den) else 0.0
    except (ValueError, AttributeError):
        return 0.0


def probe(ffprobe, path):
    """Reads duration, dimensions, fps and audio presence with ffprobe."""
    if not os.path.isfile(path):
        raise ValueError("file not found")
    res = subprocess.run(
        [ffprobe, "-v", "error", "-show_entries",
         "stream=codec_type,width,height,avg_frame_rate,r_frame_rate:format=duration",
         "-of", "json", path],
        capture_output=True, text=True, creationflags=NO_WINDOW,
    )
    if res.returncode != 0:
        raise ValueError(res.stderr.strip().splitlines()[-1] if res.stderr.strip()
                         else "unrecognized format")
    info = json.loads(res.stdout)
    streams = info.get("streams", [])
    video = next((s for s in streams if s.get("codec_type") == "video"), None)
    if video is None:
        raise ValueError("no video stream found")
    return {
        "path": path,
        "duration": float(info["format"]["duration"]),
        "width": video.get("width") or 0,
        "height": video.get("height") or 0,
        "fps": parse_rate(video.get("avg_frame_rate")) or parse_rate(video.get("r_frame_rate")),
        "has_audio": any(s.get("codec_type") == "audio" for s in streams),
        "size": os.path.getsize(path),
    }


def quality_name(crf):
    for limit, name in ((20, "very high"), (24, "high"), (28, "good"), (32, "medium"), (37, "low")):
        if crf <= limit:
            return name
    return "very low"


def fmt_time(seconds):
    m, s = divmod(int(round(seconds)), 60)
    h, m = divmod(m, 60)
    return f"{h}:{m:02d}:{s:02d}" if h else f"{m}:{s:02d}"


def fmt_mb(num_bytes):
    mb = num_bytes / MB
    return f"{mb:.2f} MB" if mb < 10 else f"{mb:.1f} MB"


# ---------------------------------------------------------------- estimate
class SizeEstimator(QObject):
    """Encodes a few samples in the background and returns the video's bytes per second."""
    done = Signal(float, bool)   # bytes per second, exact estimate (video encoded whole)
    failed = Signal(str)

    def __init__(self, ffmpeg, parent=None):
        super().__init__(parent)
        self.ffmpeg = ffmpeg
        self.gen = 0
        self.proc = None
        self.queue = []
        self.tmpdir = None
        self.total_bytes = 0
        self.total_secs = 0.0
        self.exact = False

    def start(self, path, segments, video_args):
        self.stop()
        self.tmpdir = tempfile.mkdtemp(prefix="video_estimate_")
        self.total_bytes, self.total_secs = 0, 0.0
        self.exact = len(segments) == 1 and segments[0][0] == 0
        self.queue = []
        for i, (start, length) in enumerate(segments):
            out = os.path.join(self.tmpdir, f"sample{i}.mp4")
            args = (["-y", "-hide_banner", "-v", "error", "-ss", f"{start:.3f}", "-t", f"{length:.3f}",
                     "-i", path] + video_args + ["-an", out])
            self.queue.append((args, out, length))
        self._run_next()

    def stop(self):
        self.gen += 1
        self.queue = []
        if self.proc and self.proc.state() != QProcess.NotRunning:
            self.proc.kill()
            self.proc.waitForFinished(3000)
        self.proc = None
        self._cleanup()

    def _run_next(self):
        if not self.queue:
            bps = self.total_bytes / self.total_secs if self.total_secs else 0.0
            self._cleanup()
            self.done.emit(bps, self.exact)
            return
        args, out, secs = self.queue.pop(0)
        gen = self.gen
        proc = QProcess(self)
        proc.finished.connect(lambda code, _st, p=proc, o=out, s=secs, g=gen: self._on_finished(g, p, o, s, code))
        self.proc = proc
        proc.start(self.ffmpeg, args)

    def _on_finished(self, gen, proc, out, secs, code):
        err = bytes(proc.readAllStandardError()).decode(errors="ignore").strip()[-400:]
        if proc is self.proc:
            self.proc = None
        proc.deleteLater()
        if gen != self.gen:
            return  # stale estimate, cancelled
        if code != 0 or not os.path.exists(out):
            self._cleanup()
            self.failed.emit(err or "ffmpeg error")
            return
        self.total_bytes += os.path.getsize(out)
        self.total_secs += secs
        self._run_next()

    def _cleanup(self):
        if self.tmpdir:
            shutil.rmtree(self.tmpdir, ignore_errors=True)
            self.tmpdir = None


# ---------------------------------------------------------------- window
class MainWindow(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Video Compressor")
        self.setAcceptDrops(True)
        self.resize(640, 720)

        self.ffmpeg = find_tool("ffmpeg")
        self.ffprobe = find_tool("ffprobe")
        self.media = None
        self.est_bps = None          # estimated bytes/s of the video track
        self.est_exact = False
        self.fit = None              # state of the "Fit to limit" search
        self.est_cache = {}          # video settings -> bytes/s already estimated
        self._programmatic = False
        self.encoding = False
        self.proc = None
        self.cancelled = False
        self.current_output = None
        self.out_buf = ""
        self.stderr_tail = []

        self.estimator = SizeEstimator(self.ffmpeg, self)
        self.estimator.done.connect(self.on_estimate_done)
        self.estimator.failed.connect(self.on_estimate_failed)
        self.est_timer = QTimer(self)
        self.est_timer.setSingleShot(True)
        self.est_timer.setInterval(600)
        self.est_timer.timeout.connect(self.run_estimate)

        # --- Source ---
        self.in_edit = QLineEdit()
        self.in_edit.setPlaceholderText("Drop a video here or click Browse…")
        self.in_edit.editingFinished.connect(self.on_input_edited)
        self.in_btn = QPushButton("Browse…")
        self.in_btn.clicked.connect(self.pick_input)
        in_row = QHBoxLayout()
        in_row.addWidget(self.in_edit)
        in_row.addWidget(self.in_btn)
        self.src_info = QLabel("No video loaded.")
        self.src_info.setWordWrap(True)
        src_box = QGroupBox("Source video")
        src_layout = QVBoxLayout(src_box)
        src_layout.addLayout(in_row)
        src_layout.addWidget(self.src_info)

        # --- Quality settings ---
        self.crf_slider = QSlider(Qt.Horizontal)
        self.crf_slider.setRange(CRF_MIN, CRF_MAX)
        self.crf_slider.setValue(CRF_DEFAULT)
        self.crf_slider.setInvertedAppearance(True)   # right = higher quality
        self.crf_slider.setInvertedControls(True)
        self.crf_label = QLabel()
        slider_row = QHBoxLayout()
        slider_row.addWidget(QLabel("Smaller file"))
        slider_row.addWidget(self.crf_slider, 1)
        slider_row.addWidget(QLabel("Higher quality"))

        self.res_combo = QComboBox()
        self.fps_combo = QComboBox()
        self.audio_combo = QComboBox()
        for label, val in AUDIO:
            self.audio_combo.addItem(label, val)
        self.preset_combo = QComboBox()
        for label, val in PRESETS:
            self.preset_combo.addItem(label, val)

        form = QFormLayout()
        form.addRow("Quality:", slider_row)
        form.addRow("", self.crf_label)
        form.addRow("Resolution:", self.res_combo)
        form.addRow("Frame rate:", self.fps_combo)
        form.addRow("Audio:", self.audio_combo)
        form.addRow("Speed:", self.preset_combo)
        self.settings_box = QGroupBox("Settings")
        self.settings_box.setLayout(form)
        self.settings_box.setEnabled(False)

        self.crf_slider.valueChanged.connect(self.on_settings_changed)
        for combo in (self.res_combo, self.fps_combo, self.audio_combo, self.preset_combo):
            combo.currentIndexChanged.connect(self.on_settings_changed)

        # --- Size preview ---
        self.size_label = QLabel("—")
        font = self.size_label.font()
        font.setPointSize(font.pointSize() + 10)
        font.setBold(True)
        self.size_label.setFont(font)
        self.size_detail = QLabel("Load a video to see the estimate.")
        self.size_detail.setWordWrap(True)

        self.limit_spin = QDoubleSpinBox()
        self.limit_spin.setRange(0.5, 4000)
        self.limit_spin.setDecimals(1)
        self.limit_spin.setValue(10)
        self.limit_spin.setSuffix(" MB")
        self.limit_spin.valueChanged.connect(self.update_limit_status)
        self.limit_status = QLabel()
        self.fit_btn = QPushButton("Fit quality to limit")
        self.fit_btn.setEnabled(False)
        self.fit_btn.clicked.connect(self.fit_to_limit)
        limit_row = QHBoxLayout()
        limit_row.addWidget(QLabel("Limit:"))
        limit_row.addWidget(self.limit_spin)
        limit_row.addWidget(self.limit_status, 1)
        limit_row.addWidget(self.fit_btn)

        preview_box = QGroupBox("Estimated output size")
        preview_layout = QVBoxLayout(preview_box)
        preview_layout.addWidget(self.size_label)
        preview_layout.addWidget(self.size_detail)
        preview_layout.addLayout(limit_row)

        # --- Output and start ---
        self.out_edit = QLineEdit()
        self.out_btn = QPushButton("Browse…")
        self.out_btn.clicked.connect(self.pick_output)
        out_row = QHBoxLayout()
        out_row.addWidget(QLabel("Save as:"))
        out_row.addWidget(self.out_edit)
        out_row.addWidget(self.out_btn)

        self.progress = QProgressBar()
        self.progress.setRange(0, 100)
        self.start_btn = QPushButton("Compress")
        self.start_btn.setEnabled(False)
        self.start_btn.clicked.connect(self.start)
        self.cancel_btn = QPushButton("Cancel")
        self.cancel_btn.setEnabled(False)
        self.cancel_btn.clicked.connect(self.cancel)
        self.open_btn = QPushButton("Open folder")
        self.open_btn.setEnabled(False)
        self.open_btn.clicked.connect(self.open_folder)
        btn_row = QHBoxLayout()
        btn_row.addWidget(self.start_btn)
        btn_row.addWidget(self.cancel_btn)
        btn_row.addStretch()
        btn_row.addWidget(self.open_btn)

        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(500)
        self.log.setMaximumHeight(110)

        layout = QVBoxLayout(self)
        layout.addWidget(src_box)
        layout.addWidget(self.settings_box)
        layout.addWidget(preview_box)
        layout.addLayout(out_row)
        layout.addWidget(self.progress)
        layout.addLayout(btn_row)
        layout.addWidget(self.log)

        self.update_crf_label()
        if not (self.ffmpeg and self.ffprobe):
            self.src_info.setText("⚠ ffmpeg/ffprobe not found. Put them in the 'ffmpeg' folder "
                                  "or install them (e.g. 'winget install Gyan.FFmpeg') and restart the program.")
            self.in_edit.setEnabled(False)
            self.in_btn.setEnabled(False)

    # ------------------------------------------------ loading the video
    def pick_input(self):
        path, _ = QFileDialog.getOpenFileName(self, "Choose a video", "", VIDEO_FILTER)
        if path:
            self.set_input(path)

    def on_input_edited(self):
        path = self.in_edit.text().strip().strip('"')
        if path and (not self.media or os.path.abspath(path) != os.path.abspath(self.media["path"])):
            self.set_input(path)

    def dragEnterEvent(self, e):
        if e.mimeData().hasUrls() and not self.encoding:
            e.acceptProposedAction()

    def dropEvent(self, e):
        urls = e.mimeData().urls()
        if urls:
            self.set_input(urls[0].toLocalFile())

    def set_input(self, path):
        if not path or not self.ffprobe or self.encoding:
            return
        self.in_edit.setText(path)
        self.estimator.stop()
        try:
            media = probe(self.ffprobe, path)
            if media["duration"] <= 0:
                raise ValueError("invalid duration")
        except Exception as e:
            self.media = None
            self.settings_box.setEnabled(False)
            self.start_btn.setEnabled(False)
            self.fit_btn.setEnabled(False)
            self.src_info.setText(f"⚠ Could not read the video: {e}")
            return

        self.media = media
        self.est_bps = None
        self.est_cache = {}
        self.fit = None
        p = Path(path)
        self.out_edit.setText(str(p.with_name(p.stem + "_compressed.mp4")))
        self.src_info.setText(
            f"Duration {fmt_time(media['duration'])} · {media['width']}×{media['height']} · "
            f"{media['fps']:.0f} fps · audio {'yes' if media['has_audio'] else 'no'} · "
            f"{fmt_mb(media['size'])}")

        # The options offered depend on the video: no upscaling and no higher fps
        short_side = min(media["width"], media["height"])
        self._programmatic = True
        self.res_combo.clear()
        self.res_combo.addItem(f"Original ({short_side}p)", 0)
        for r in RES_STEPS:
            if r < short_side:
                self.res_combo.addItem(f"{r}p", r)
        self.fps_combo.clear()
        self.fps_combo.addItem(f"Original ({media['fps']:.0f} fps)", 0)
        for f in FPS_STEPS:
            if f < media["fps"] - 0.5:
                self.fps_combo.addItem(f"{f} fps", f)
        self.audio_combo.setEnabled(media["has_audio"])
        self._programmatic = False

        self.settings_box.setEnabled(True)
        self.start_btn.setEnabled(True)
        self.open_btn.setEnabled(False)
        self.on_settings_changed()

    # ------------------------------------------------ settings
    def crf(self):
        return self.crf_slider.value()

    def audio_kbps(self):
        return self.audio_combo.currentData() if self.media and self.media["has_audio"] else 0

    def video_args(self):
        m = self.media
        filters = []
        target = self.res_combo.currentData()
        if target:
            # scale the short side, so it also works with vertical videos
            filters.append(f"scale=-2:{target}" if m["width"] >= m["height"] else f"scale={target}:-2")
        elif m["width"] % 2 or m["height"] % 2:
            filters.append("scale=trunc(iw/2)*2:trunc(ih/2)*2")  # H.264 needs even dimensions
        fps = self.fps_combo.currentData()
        if fps:
            filters.append(f"fps={fps}")
        args = ["-vf", ",".join(filters)] if filters else []
        return args + ["-c:v", "libx264", "-preset", self.preset_combo.currentData(),
                       "-crf", str(self.crf()), "-pix_fmt", "yuv420p"]

    def update_crf_label(self):
        self.crf_label.setText(f"{quality_name(self.crf()).capitalize()} quality (CRF {self.crf()})")

    def on_settings_changed(self, *_):
        if not self._programmatic:
            self.fit = None   # a manual change interrupts the search
        self.update_crf_label()
        if not self.media or self.encoding:
            return
        self.estimator.stop()
        self.size_label.setStyleSheet("color: gray;")
        self.size_detail.setText("Updating estimate…")
        self.fit_btn.setEnabled(False)
        self.est_timer.start()

    # ------------------------------------------------ size estimate
    def run_estimate(self):
        if not self.media or self.encoding:
            return
        dur = self.media["duration"]
        if dur <= WHOLE_IF_SHORTER:
            segments = [(0.0, dur)]
        else:
            segments = [(max(0.0, dur * (i + 0.5) / SAMPLE_COUNT - SAMPLE_LEN / 2), SAMPLE_LEN)
                        for i in range(SAMPLE_COUNT)]
        key = tuple(self.video_args())
        if key in self.est_cache:
            self._pending_key = None
            self.on_estimate_done(*self.est_cache[key])
            return
        self._pending_key = key
        self.size_detail.setText("Searching for the best quality under the limit…" if self.fit else
                                 "Estimating (encoding a few short samples)…")
        self.estimator.start(self.media["path"], segments, list(key))

    def estimated_bytes(self):
        dur = self.media["duration"]
        return (self.est_bps + self.audio_kbps() * 1000 / 8) * dur * CONTAINER_OVERHEAD

    def on_estimate_done(self, bps, exact):
        key = getattr(self, "_pending_key", None)
        if key is not None:
            self.est_cache[key] = (bps, exact)
            self._pending_key = None
        self.est_bps = bps
        self.est_exact = exact
        total = self.estimated_bytes()
        self.size_label.setStyleSheet("")
        self.size_label.setText(f"{'' if exact else '≈ '}{fmt_mb(total)}")
        accuracy = "exact estimate" if exact else "sample-based estimate, roughly ±10%"
        self.size_detail.setText(
            f"Video ≈ {bps * 8 / 1000:.0f} kbps · audio {self.audio_kbps()} kbps · "
            f"{total / self.media['size']:.0%} of the original ({accuracy})")
        self.update_limit_status()
        if self.fit:
            self.fit_step()
        self.fit_btn.setEnabled(self.fit is None)

    def on_estimate_failed(self, msg):
        self.fit = None
        self._pending_key = None
        self.est_bps = None
        self.size_label.setStyleSheet("")
        self.size_label.setText("—")
        self.size_detail.setText(f"⚠ Estimate failed: {msg}")
        self.update_limit_status()

    def update_limit_status(self, *_):
        if not self.media or self.est_bps is None:
            self.limit_status.setText("")
            return
        if self.estimated_bytes() <= self.limit_spin.value() * MB:
            self.limit_status.setText("✔ Under the limit")
            self.limit_status.setStyleSheet("color: #2e7d32; font-weight: bold;")
        else:
            self.limit_status.setText("✖ Over the limit")
            self.limit_status.setStyleSheet("color: #c62828; font-weight: bold;")

    # ------------------------------------------------ fit to limit
    def fit_to_limit(self):
        """Searches for the lowest CRF (= highest quality) that stays under the limit."""
        if not self.media or self.est_bps is None:
            return
        target = self.limit_spin.value() * MB * LIMIT_MARGIN
        audio_bytes = self.audio_kbps() * 1000 / 8 * self.media["duration"] * CONTAINER_OVERHEAD
        if audio_bytes >= target:
            QMessageBox.information(self, "Limit too low",
                                    "The audio alone already exceeds the limit: lower or remove it.")
            return
        self.fit = {"lo": CRF_MIN, "hi": CRF_MAX, "best": None, "points": [], "left": 8,
                    "target": target}
        self.fit_btn.setEnabled(False)
        self.fit_step()

    def set_crf(self, value):
        self._programmatic = True
        self.crf_slider.setValue(value)
        self._programmatic = False

    def fit_step(self):
        f = self.fit
        crf, size = self.crf(), self.estimated_bytes()
        f["points"].append((crf, size))
        if size <= f["target"]:
            f["best"] = crf if f["best"] is None else min(f["best"], crf)
            f["hi"] = min(f["hi"], crf - 1)     # try a higher quality
        else:
            f["lo"] = max(f["lo"], crf + 1)     # needs more compression
        f["left"] -= 1

        if f["lo"] > f["hi"] or f["left"] <= 0:
            self.fit = None
            if f["best"] is not None:
                if f["best"] != crf:
                    self.set_crf(f["best"])
            elif f["lo"] > CRF_MAX:
                QMessageBox.information(
                    self, "Limit not reachable",
                    "Even at the lowest quality the file exceeds the limit.\n"
                    "Try reducing the resolution, frame rate or audio.")
            else:
                self.set_crf(min(f["lo"], CRF_MAX))
            return

        # guess the next CRF: size drops roughly exponentially with CRF
        slope = -math.log(2) / 6   # x264 rule of thumb: +6 CRF ≈ half the size
        if len(f["points"]) >= 2:
            (c1, s1), (c2, s2) = f["points"][-2:]
            if c1 != c2 and s1 > 0 and s2 > 0:
                measured = (math.log(s2) - math.log(s1)) / (c2 - c1)
                if measured < 0:
                    slope = measured
        nxt = round(crf + (math.log(f["target"]) - math.log(size)) / slope)
        self.set_crf(max(f["lo"], min(f["hi"], nxt)))

    # ------------------------------------------------ output
    def pick_output(self):
        path, _ = QFileDialog.getSaveFileName(self, "Save as", self.out_edit.text(), "MP4 (*.mp4)")
        if path:
            if not path.lower().endswith(".mp4"):
                path += ".mp4"
            self.out_edit.setText(path)

    def open_folder(self):
        if self.current_output:
            QDesktopServices.openUrl(QUrl.fromLocalFile(os.path.dirname(self.current_output)))

    # ------------------------------------------------ compression
    def start(self):
        if not self.media:
            return
        inp = self.media["path"]
        out = self.out_edit.text().strip().strip('"')
        if not out:
            QMessageBox.warning(self, "Error", "Choose where to save the file.")
            return
        if os.path.abspath(inp) == os.path.abspath(out):
            QMessageBox.warning(self, "Error", "The output file must be different from the input file.")
            return

        self.est_timer.stop()
        self.estimator.stop()
        ak = self.audio_kbps()
        args = (["-y", "-hide_banner", "-nostats", "-progress", "pipe:1", "-i", inp]
                + self.video_args()
                + (["-c:a", "aac", "-b:a", f"{ak}k"] if ak else ["-an"])
                + ["-movflags", "+faststart", out])

        self.cancelled = False
        self.current_output = out
        self.out_buf = ""
        self.stderr_tail = []
        self.progress.setValue(0)
        self.log.clear()
        self.log.appendPlainText(f"Compressing: CRF {self.crf()} · {self.res_combo.currentText()} · "
                                 f"{self.fps_combo.currentText()} · audio {ak} kbps · "
                                 f"{self.preset_combo.currentText().lower()}")
        self.set_running(True)
        self.proc = QProcess(self)
        self.proc.readyReadStandardOutput.connect(self.on_stdout)
        self.proc.readyReadStandardError.connect(self.on_stderr)
        self.proc.finished.connect(self.on_finished)
        self.proc.start(self.ffmpeg, args)

    def on_stdout(self):
        self.out_buf += bytes(self.proc.readAllStandardOutput()).decode(errors="ignore")
        *lines, self.out_buf = self.out_buf.split("\n")
        for line in lines:
            line = line.strip()
            if line.startswith(("out_time_us=", "out_time_ms=")):
                val = line.split("=", 1)[1]
                if val.isdigit():
                    frac = min(int(val) / 1e6 / self.media["duration"], 1.0)
                    self.progress.setValue(int(frac * 100))

    def on_stderr(self):
        text = bytes(self.proc.readAllStandardError()).decode(errors="ignore")
        self.stderr_tail = (self.stderr_tail + text.splitlines())[-30:]

    def on_finished(self, code, _status):
        if self.cancelled:
            return
        self.set_running(False)
        if code != 0:
            self.log.appendPlainText("\n".join(self.stderr_tail))
            QMessageBox.critical(self, "Error", "ffmpeg returned an error. See the log for details.")
            return
        self.progress.setValue(100)
        size = os.path.getsize(self.current_output)
        msg = f"Done! {fmt_mb(size)} → {self.current_output}"
        if self.est_bps is not None:
            msg += f"\n(estimate was {fmt_mb(self.estimated_bytes())})"
        self.log.appendPlainText(msg)
        self.open_btn.setEnabled(True)
        if size > self.limit_spin.value() * MB:
            QMessageBox.warning(self, "Warning",
                                f"The file is {fmt_mb(size)}, over the "
                                f"{self.limit_spin.value()} MB limit.\n"
                                "Use \"Fit quality to limit\" or lower the quality a bit and try again.")

    def cancel(self):
        self.cancelled = True
        if self.proc and self.proc.state() != QProcess.NotRunning:
            self.proc.kill()
            self.proc.waitForFinished(3000)
        if self.current_output and os.path.exists(self.current_output):
            try:
                os.remove(self.current_output)
            except OSError:
                pass
        self.progress.setValue(0)
        self.log.appendPlainText("Cancelled.")
        self.set_running(False)

    def set_running(self, running):
        self.encoding = running
        self.start_btn.setEnabled(not running)
        self.cancel_btn.setEnabled(running)
        self.fit_btn.setEnabled(not running and self.est_bps is not None and self.fit is None)
        if running:
            self.open_btn.setEnabled(False)
        for w in (self.in_edit, self.in_btn, self.out_edit, self.out_btn,
                  self.settings_box, self.limit_spin):
            w.setEnabled(not running)

    def closeEvent(self, e):
        self.estimator.stop()
        if self.proc and self.proc.state() != QProcess.NotRunning:
            self.cancel()
        e.accept()


if __name__ == "__main__":
    app = QApplication(sys.argv)
    win = MainWindow()
    win.show()
    if len(sys.argv) > 1:
        win.set_input(sys.argv[1])
    sys.exit(app.exec())
