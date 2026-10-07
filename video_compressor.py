#!/usr/bin/env python3
"""
Video Compressor - Qt GUI to compress videos at a chosen quality,
with a preview of the final file size.

The estimate is made by actually encoding a few short samples of the video
with the chosen settings, so it is reliable even with very different kinds of
content (screen recordings, camera footage, animations...).

Any video FFmpeg can read is accepted as input. The output can be MP4, MKV,
WebM, MOV or AVI, with H.264, H.265/HEVC, AV1, VP9, H.266/VVC or MPEG-4 video
(including NVIDIA, AMD and Intel GPU encoders, when the PC supports them) and
AAC, Opus, MP3 or Vorbis audio.

Requirements:
    - Python 3.9+
    - PySide6         ->  pip install PySide6
    - ffmpeg/ffprobe  ->  bundled in the executable; when running from source
                          they are taken from the ffmpeg/ folder or from PATH
                          (winget install Gyan.FFmpeg)

Run:
    python video_compressor.py
"""

import ctypes
import json
import math
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

if sys.platform == "win32":
    import winreg
    from ctypes import wintypes

from PySide6.QtCore import QObject, QPointF, QProcess, Qt, QTimer, QUrl, Signal
from PySide6.QtGui import QColor, QDesktopServices, QIcon, QPainter, QPixmap, QPolygonF
from PySide6.QtWidgets import (
    QApplication, QComboBox, QDoubleSpinBox, QFileDialog, QFormLayout, QGroupBox,
    QHBoxLayout, QLabel, QLineEdit, QMessageBox, QPlainTextEdit, QProgressBar,
    QPushButton, QSlider, QVBoxLayout, QWidget,
)

VIDEO_EXTENSIONS = (
    "mp4 m4v mkv mov qt avi webm wmv asf flv f4v ts mts m2ts m2t mpg mpeg mpe m1v m2v vob evo "
    "3gp 3g2 mxf ogv ogm dv divx xvid rm rmvb nut y4m h264 264 h265 265 hevc ivf obu gif apng "
    "amv mjpeg mjpg wtv dvr-ms"
).split()
VIDEO_FILTER = f"Videos ({' '.join('*.' + e for e in VIDEO_EXTENSIONS)});;All files (*)"
NO_WINDOW = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
MB = 1024 * 1024

SAMPLE_COUNT, SAMPLE_LEN = 4, 4.0            # samples used for the estimate
WHOLE_IF_SHORTER = 20.0                      # shorter videos are encoded whole (exact estimate)
CONTAINER_OVERHEAD = 1.01                    # ~1% for the container
LIMIT_MARGIN = 0.94                          # safety margin for "Fit to limit"

SPEED_LEVELS = [("Balanced", "balanced"), ("Fast", "fast"), ("Best quality (slow)", "best")]
KEYFRAME_INTERVALS = [("Every 10 s (smallest file)", 10), ("Every 5 s", 5),
                      ("Every 2 s (editing, streaming)", 2), ("Every 1 s", 1)]
AUDIO_BITRATES, DEFAULT_AUDIO_KBPS = [64, 96, 128, 160, 192], 96
RES_STEPS = [1080, 720, 540, 480, 360]
FPS_STEPS = [30, 24, 15]


# ---------------------------------------------------------------- codecs and formats
def presets(fast, balanced, best, option="-preset"):
    return {"fast": [option, fast], "balanced": [option, balanced], "best": [option, best]}


def crf(q):
    return ["-crf", str(q)]


def nvenc_cq(q):
    return ["-rc", "vbr", "-cq", str(q), "-b:v", "0"]


def amf_qp(*frame_types):
    return lambda q: ["-rc", "cqp"] + [a for t in frame_types for a in (f"-qp_{t}", str(q))]


def qsv_icq(q):
    return ["-global_quality", str(q)]


X26X_SPEEDS = presets("veryfast", "medium", "slow")
NVENC_SPEEDS = presets("p2", "p5", "p7")
AMF_SPEEDS = presets("speed", "balanced", "quality", "-quality")
QSV_SPEEDS = presets("veryfast", "medium", "veryslow")


@dataclass
class VideoCodec:
    label: str
    encoder: str
    family: str                  # h264, hevc, av1, vp9, vvc or mpeg4
    quality: tuple               # (best, worst, default) on the encoder's scale, lower = better
    q_name: str                  # name of the quality parameter shown in the UI
    q_args: Callable             # quality value -> ffmpeg args
    speeds: dict = None          # "fast"/"balanced"/"best" -> ffmpeg args, None = no presets
    pix_fmt: str = "yuv420p"
    halving: float = 6           # quality steps that roughly halve the size (first guess for "Fit to limit")
    extra: tuple = ()
    vendor: str = None           # GPU encoder of this vendor (see VENDORS), None = software
    gop_args: Callable = lambda frames, secs: ["-g", str(frames)]   # keyframe interval

    @property
    def hardware(self):
        return self.vendor is not None

    def args(self, quality, speed, gop=None):
        """gop: (frames, seconds) between keyframes, None = encoder default."""
        args = ["-c:v", self.encoder] + self.q_args(quality)
        if self.speeds:
            args += self.speeds[speed]
        if gop:
            args += self.gop_args(*gop)
        return args + list(self.extra) + ["-pix_fmt", self.pix_fmt]


@dataclass
class AudioCodec:
    label: str
    encoder: str
    name: str
    extra: tuple = ()

    def args(self, kbps):
        return ["-c:a", self.encoder, "-b:a", f"{kbps}k"] + list(self.extra)


@dataclass
class Format:
    label: str
    ext: str
    video: set                   # accepted video codec families
    audio: set                   # accepted audio codec names


# video codecs shown in the menu: (family, name, note)
FAMILIES = [("h264", "H.264 / AVC", ""), ("hevc", "H.265 / HEVC", ""), ("av1", "AV1", ""), ("vp9", "VP9", ""),
            ("vvc", "H.266 / VVC", "very slow"), ("mpeg4", "MPEG-4 Part 2 / Xvid", "old devices")]
FAMILY_NAMES = {key: name for key, name, _ in FAMILIES}

# GPU vendors, in order of preference for the automatic choice: (generic name, encoder, PCI vendor id)
VENDORS = {"nvidia": ("NVIDIA GPU", "NVENC", "10DE"),
           "intel": ("Intel GPU", "Quick Sync", "8086"),
           "amd": ("AMD GPU", "AMF", "1002")}

# label = name of the encoder shown in the UI
VIDEO_CODECS = [
    VideoCodec("x264", "libx264", "h264", (18, 45, 28), "CRF", crf, X26X_SPEEDS),
    # closed GOP: every keyframe is an IDR, a clean starting point for seeking and editing
    VideoCodec("x265", "libx265", "hevc", (18, 45, 30), "CRF", crf, X26X_SPEEDS,
               extra=("-x265-params", "log-level=error:open-gop=0")),
    VideoCodec("SVT-AV1", "libsvtav1", "av1", (20, 63, 35), "CRF", crf,
               presets("10", "8", "6"), halving=8),
    VideoCodec("libvpx", "libvpx-vp9", "vp9", (15, 63, 33), "CRF", lambda q: crf(q) + ["-b:v", "0"],
               presets("5", "4", "2", "-cpu-used"), halving=8, extra=("-row-mt", "1", "-deadline", "good")),
    VideoCodec("VVenC", "libvvenc", "vvc", (20, 63, 32), "QP",
               lambda q: ["-qp", str(q)], presets("faster", "fast", "medium"), pix_fmt="yuv420p10le",
               extra=("-vvenc-params", "DecodingRefreshType=idr"),
               gop_args=lambda frames, secs: ["-period", str(secs)]),   # VVenC ignores -g
    VideoCodec("Xvid", "libxvid", "mpeg4", (2, 31, 5), "Q", lambda q: ["-q:v", str(q)], halving=5),
    VideoCodec("NVENC", "h264_nvenc", "h264", (18, 51, 28), "CQ", nvenc_cq, NVENC_SPEEDS, vendor="nvidia"),
    VideoCodec("NVENC", "hevc_nvenc", "hevc", (18, 51, 30), "CQ", nvenc_cq, NVENC_SPEEDS, vendor="nvidia"),
    VideoCodec("NVENC", "av1_nvenc", "av1", (20, 63, 32), "CQ", nvenc_cq, NVENC_SPEEDS, halving=8,
               vendor="nvidia"),
    VideoCodec("AMF", "h264_amf", "h264", (18, 51, 28), "QP", amf_qp("i", "p", "b"), AMF_SPEEDS, vendor="amd"),
    VideoCodec("AMF", "hevc_amf", "hevc", (18, 51, 30), "QP", amf_qp("i", "p"), AMF_SPEEDS, vendor="amd"),
    VideoCodec("AMF", "av1_amf", "av1", (80, 255, 140), "QP", amf_qp("i", "p", "b"), AMF_SPEEDS, halving=32,
               vendor="amd"),
    VideoCodec("Quick Sync", "h264_qsv", "h264", (18, 51, 28), "Q", qsv_icq, QSV_SPEEDS, pix_fmt="nv12",
               vendor="intel"),
    VideoCodec("Quick Sync", "hevc_qsv", "hevc", (18, 51, 30), "Q", qsv_icq, QSV_SPEEDS, pix_fmt="nv12",
               vendor="intel"),
    VideoCodec("Quick Sync", "av1_qsv", "av1", (18, 51, 30), "Q", qsv_icq, QSV_SPEEDS, pix_fmt="nv12",
               vendor="intel"),
]

AUDIO_CODECS = [
    AudioCodec("AAC", "aac", "aac"),
    # libopus only accepts the standard channel layouts: e.g. 5.1(side) becomes 5.1
    AudioCodec("Opus", "libopus", "opus", ("-af", "aformat=channel_layouts=mono|stereo|3.0|quad|5.0|5.1|6.1|7.1")),
    AudioCodec("MP3", "libmp3lame", "mp3"),
    AudioCodec("Vorbis", "libvorbis", "vorbis"),
]

FORMATS = [
    Format("MP4", "mp4", {"h264", "hevc", "av1", "vp9", "vvc", "mpeg4"}, {"aac", "opus", "mp3"}),
    Format("MKV", "mkv", {"h264", "hevc", "av1", "vp9", "vvc", "mpeg4"}, {"aac", "opus", "mp3", "vorbis"}),
    Format("WebM", "webm", {"av1", "vp9"}, {"opus", "vorbis"}),
    Format("MOV", "mov", {"h264", "hevc"}, {"aac", "mp3"}),
    Format("AVI", "avi", {"mpeg4"}, {"mp3"}),
]


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
         "stream=index,codec_type,width,height,avg_frame_rate,r_frame_rate,duration:"
         "stream_disposition=attached_pic:stream_side_data=rotation:format=duration",
         "-of", "json", path],
        capture_output=True, text=True, creationflags=NO_WINDOW,
    )
    if res.returncode != 0:
        raise ValueError(res.stderr.strip().splitlines()[-1] if res.stderr.strip()
                         else "unrecognized format")
    info = json.loads(res.stdout)
    streams = info.get("streams", [])
    # cover art is reported as a one-frame video stream: skip it
    video = next((s for s in streams if s.get("codec_type") == "video"
                  and not s.get("disposition", {}).get("attached_pic")), None)
    if video is None:
        raise ValueError("no video stream found")
    duration = info.get("format", {}).get("duration") or video.get("duration")
    if not duration:
        raise ValueError("unknown duration")
    width, height = video.get("width") or 0, video.get("height") or 0
    # phone videos are often stored landscape with a rotation flag: use the size as displayed
    rotation = next((d["rotation"] for d in video.get("side_data_list", []) if "rotation" in d), 0)
    if round(abs(float(rotation))) % 180 == 90:
        width, height = height, width
    return {
        "path": path,
        "duration": float(duration),
        "width": width,
        "height": height,
        "video_index": video["index"],
        "fps": parse_rate(video.get("avg_frame_rate")) or parse_rate(video.get("r_frame_rate")),
        "has_audio": any(s.get("codec_type") == "audio" for s in streams),
        "size": os.path.getsize(path),
    }


def quality_name(q, best, worst):
    frac = (q - best) / (worst - best)
    for limit, name in ((0.08, "very high"), (0.23, "high"), (0.38, "good"), (0.53, "medium"), (0.71, "low")):
        if frac <= limit:
            return name
    return "very low"


def fmt_time(seconds):
    m, s = divmod(int(round(seconds)), 60)
    h, m = divmod(m, 60)
    return f"{h}:{m:02d}:{s:02d}" if h else f"{m}:{s:02d}"


def fmt_mb(num_bytes):
    mb = num_bytes / MB
    return f"{mb:.2f} MB" if mb < 10 else f"{mb:.1f} MB"


def fill_combo(combo, items, keep=True):
    """Replaces the items of a combo box, keeping the current choice when it is still there (if keep)."""
    current = combo.currentText()
    combo.blockSignals(True)
    combo.clear()
    for label, data in items:
        combo.addItem(label, data)
    combo.setCurrentIndex(max(0, combo.findText(current)) if keep else 0)
    combo.blockSignals(False)


def gpu_names():
    """Name of the GPU of each vendor in this PC, e.g. {"nvidia": "NVIDIA GeForce RTX 3080"}."""
    names = {}
    if sys.platform != "win32":
        return names

    class DisplayDevice(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("DeviceName", wintypes.WCHAR * 32),
                    ("DeviceString", wintypes.WCHAR * 128), ("StateFlags", wintypes.DWORD),
                    ("DeviceID", wintypes.WCHAR * 128), ("DeviceKey", wintypes.WCHAR * 128)]

    i = 0
    while True:
        dev = DisplayDevice(cb=ctypes.sizeof(DisplayDevice))
        if not ctypes.windll.user32.EnumDisplayDevicesW(None, i, ctypes.byref(dev), 0):
            break
        i += 1
        for vendor, (_, _, pci_id) in VENDORS.items():   # virtual adapters (no PCI id) are skipped
            if f"VEN_{pci_id}" in dev.DeviceID.upper():
                names.setdefault(vendor, dev.DeviceString)
    return names


def cpu_name():
    if sys.platform == "win32":
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
                return " ".join(winreg.QueryValueEx(key, "ProcessorNameString")[0].split())
        except OSError:
            pass
    return platform.processor() or "CPU"


def bolt_icon(color=None):
    """Lightning bolt that marks codecs with hardware acceleration (transparent if color is None)."""
    pix = QPixmap(16, 16)
    pix.fill(Qt.transparent)
    if color:
        p = QPainter(pix)
        p.setRenderHint(QPainter.Antialiasing)
        p.setPen(Qt.NoPen)
        p.setBrush(QColor(color))
        p.drawPolygon(QPolygonF([QPointF(x, y) for x, y in
                                 ((9.5, 0.5), (3, 9), (7.5, 9), (6, 15.5), (13, 6.5), (8.5, 6.5), (11, 0.5))]))
        p.end()
    return QIcon(pix)


# ---------------------------------------------------------------- encoders
class EncoderCheck(QObject):
    """Finds the usable encoders: software ones from 'ffmpeg -encoders', GPU ones with a tiny test encode."""
    changed = Signal()

    def __init__(self, ffmpeg, parent=None):
        super().__init__(parent)
        self.available = set()
        if not ffmpeg:
            return
        res = subprocess.run([ffmpeg, "-hide_banner", "-encoders"],
                             capture_output=True, text=True, creationflags=NO_WINDOW)
        listed = {parts[1] for parts in map(str.split, res.stdout.splitlines()) if len(parts) > 1}
        for codec in VIDEO_CODECS + AUDIO_CODECS:
            if codec.encoder not in listed:
                continue
            if getattr(codec, "hardware", False):
                self._test(ffmpeg, codec)
            else:
                self.available.add(codec.encoder)

    def _test(self, ffmpeg, codec):
        proc = QProcess(self)
        proc.finished.connect(lambda code, _st, p=proc, c=codec: self._on_tested(p, c, code))
        proc.start(ffmpeg, ["-hide_banner", "-v", "error", "-f", "lavfi", "-i", "testsrc2=s=320x240:r=30:d=0.2"]
                   + codec.args(codec.quality[2], "balanced") + ["-f", "null", "-"])

    def _on_tested(self, proc, codec, code):
        proc.deleteLater()
        if code == 0:
            self.available.add(codec.encoder)
            self.changed.emit()


# ---------------------------------------------------------------- estimate
class SizeEstimator(QObject):
    """Encodes a few samples in the background and returns the video's bytes per second."""
    done = Signal(float, bool)   # bytes per second, exact estimate (video encoded whole)
    failed = Signal(str)

    def __init__(self, ffmpeg, ffprobe, parent=None):
        super().__init__(parent)
        self.ffmpeg = ffmpeg
        self.ffprobe = ffprobe
        self.gen = 0
        self.proc = None
        self.queue = []
        self.tmpdir = None
        self.total_bytes = 0
        self.total_secs = 0.0
        self.exact = False
        self.keyint = 10.0

    def start(self, path, segments, video_args, ext, keyint):
        """keyint: seconds between keyframes in the real encode."""
        self.stop()
        self.tmpdir = tempfile.mkdtemp(prefix="video_estimate_")
        self.total_bytes, self.total_secs = 0, 0.0
        self.exact = len(segments) == 1 and segments[0][0] == 0
        self.keyint = keyint
        self.queue = []
        for i, (start, length) in enumerate(segments):
            out = os.path.join(self.tmpdir, f"sample{i}.{ext}")
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
        size = os.path.getsize(out)
        if not self.exact:
            # Every sample starts with a keyframe, while the real file has one every `keyint`
            # seconds: remove the keyframes the sample has in excess, otherwise videos with
            # little motion (where keyframes are most of the size) look much bigger than they are
            excess = math.ceil(secs / self.keyint - 1e-9) - secs / self.keyint
            size -= excess * self._first_packet_size(out)
        self.total_bytes += size
        self.total_secs += secs
        self._run_next()

    def _first_packet_size(self, path):
        res = subprocess.run([self.ffprobe, "-v", "error", "-select_streams", "v:0", "-read_intervals", "%+#1",
                              "-show_entries", "packet=size", "-of", "csv=p=0", path],
                             capture_output=True, text=True, creationflags=NO_WINDOW)
        try:
            return int(res.stdout.split()[0].strip(","))
        except (IndexError, ValueError):
            return 0

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
        self.resize(640, 800)

        self.ffmpeg = find_tool("ffmpeg")
        self.ffprobe = find_tool("ffprobe")
        self.gpus = gpu_names()
        self.cpu = cpu_name()
        self.hw_icon, self.no_icon = bolt_icon("#f5a623"), bolt_icon()
        self.media = None
        self.codec = None            # VideoCodec that will be used (from codec, acceleration and device)
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

        self.encoders = EncoderCheck(self.ffmpeg, self)
        self.encoders.changed.connect(self.refresh_codecs)
        self.estimator = SizeEstimator(self.ffmpeg, self.ffprobe, self)
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

        # --- Settings ---
        self.format_combo = QComboBox()
        for f in FORMATS:
            self.format_combo.addItem(f.label, f)
        self.vcodec_combo = QComboBox()
        self.vcodec_combo.setToolTip("⚡ = this PC has a GPU that can encode the codec (hardware acceleration)")
        self.accel_combo = QComboBox()
        for label, val in (("Automatic", "auto"), ("Manual", "manual"), ("Off (software, CPU)", "off")):
            self.accel_combo.addItem(label, val)
        self.accel_combo.setToolTip(
            "Automatic: the best GPU that can encode the chosen codec, otherwise the CPU.\n"
            "Manual: choose the GPU yourself.\n"
            "Off: always encode on the CPU (slower, but smaller files at the same quality).")
        self.device_combo = QComboBox()
        accel_row = QHBoxLayout()
        accel_row.addWidget(self.accel_combo)
        accel_row.addWidget(self.device_combo, 1)
        self.encoder_label = QLabel()   # always states codec, hardware acceleration and device
        self.encoder_label.setWordWrap(True)

        self.quality_slider = QSlider(Qt.Horizontal)
        self.quality_slider.setInvertedAppearance(True)   # right = higher quality
        self.quality_slider.setInvertedControls(True)
        self.quality_label = QLabel()
        slider_row = QHBoxLayout()
        slider_row.addWidget(QLabel("Smaller file"))
        slider_row.addWidget(self.quality_slider, 1)
        slider_row.addWidget(QLabel("Higher quality"))

        self.res_combo = QComboBox()
        self.fps_combo = QComboBox()
        self.acodec_combo = QComboBox()
        self.abitrate_combo = QComboBox()
        for kbps in AUDIO_BITRATES:
            self.abitrate_combo.addItem(f"{kbps} kbps", kbps)
        self.abitrate_combo.setCurrentIndex(AUDIO_BITRATES.index(DEFAULT_AUDIO_KBPS))
        audio_row = QHBoxLayout()
        audio_row.addWidget(self.acodec_combo, 1)
        audio_row.addWidget(self.abitrate_combo)
        self.speed_combo = QComboBox()
        for label, val in SPEED_LEVELS:
            self.speed_combo.addItem(label, val)
        self.keyint_combo = QComboBox()
        for label, val in KEYFRAME_INTERVALS:
            self.keyint_combo.addItem(label, val)
        self.keyint_combo.setToolTip(
            "Keyframes are the points where playback can start when you jump in the video.\n"
            "More frequent keyframes give faster, more precise seeking (useful for editing\n"
            "and streaming) but a bigger file, especially for videos with little motion.")

        form = QFormLayout()
        form.addRow("Format:", self.format_combo)
        form.addRow("Video codec:", self.vcodec_combo)
        form.addRow("Hardware acceleration:", accel_row)
        form.addRow("", self.encoder_label)
        form.addRow("Quality:", slider_row)
        form.addRow("", self.quality_label)
        form.addRow("Resolution:", self.res_combo)
        form.addRow("Frame rate:", self.fps_combo)
        form.addRow("Audio:", audio_row)
        form.addRow("Speed:", self.speed_combo)
        form.addRow("Keyframes:", self.keyint_combo)
        self.settings_box = QGroupBox("Settings")
        self.settings_box.setLayout(form)
        self.settings_box.setEnabled(False)

        self.format_combo.currentIndexChanged.connect(self.on_format_changed)
        for combo in (self.vcodec_combo, self.accel_combo, self.device_combo):
            combo.currentIndexChanged.connect(self.update_encoder)
        self.acodec_combo.currentIndexChanged.connect(self.on_acodec_changed)
        self.quality_slider.valueChanged.connect(self.on_settings_changed)
        for combo in (self.res_combo, self.fps_combo, self.abitrate_combo, self.speed_combo, self.keyint_combo):
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

        self.refresh_codecs()
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
        self.out_edit.setText(str(p.with_name(f"{p.stem}_compressed.{self.fmt().ext}")))
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
        self._programmatic = False
        self.update_audio_widgets()

        self.settings_box.setEnabled(True)
        self.open_btn.setEnabled(False)
        self.on_settings_changed()

    # ------------------------------------------------ settings
    def fmt(self):
        return self.format_combo.currentData()

    def quality(self):
        return self.quality_slider.value()

    def keyint(self):
        return self.keyint_combo.currentData()

    def audio_codec(self):
        return self.acodec_combo.currentData() if self.media and self.media["has_audio"] else None

    def audio_kbps(self):
        return self.abitrate_combo.currentData() if self.audio_codec() else 0

    def hw_encoders(self, family):
        """GPU encoders for a codec that work on this PC, best first."""
        return sorted((c for c in VIDEO_CODECS if c.family == family and c.vendor
                       and c.encoder in self.encoders.available), key=lambda c: list(VENDORS).index(c.vendor))

    def device_name(self, vendor):
        return self.gpus.get(vendor, VENDORS[vendor][0])

    def refresh_codecs(self):
        """Offers only the codecs that the chosen format accepts and this PC can encode."""
        fmt, available = self.fmt(), self.encoders.available
        prev_audio = self.acodec_combo.currentData()
        self._programmatic = True
        families = [(key, name, note) for key, name, note in FAMILIES if key in fmt.video
                    and any(c.family == key and c.encoder in available for c in VIDEO_CODECS)]
        fill_combo(self.vcodec_combo, [(f"{name} ({note})" if note else name, key) for key, name, note in families])
        for i, (key, _, _) in enumerate(families):
            gpus = [self.device_name(c.vendor) for c in self.hw_encoders(key)]
            self.vcodec_combo.setItemIcon(i, self.hw_icon if gpus else self.no_icon)
            self.vcodec_combo.setItemData(i, f"Hardware acceleration available on: {', '.join(gpus)}" if gpus
                                          else "Software encoding only (CPU)", Qt.ToolTipRole)
        fill_combo(self.acodec_combo, [(a.label, a) for a in AUDIO_CODECS
                                       if a.name in fmt.audio and a.encoder in available] + [("No audio", None)])
        self.update_encoder()
        if self.acodec_combo.currentData() != prev_audio:
            self.on_acodec_changed()
        self._programmatic = False

    def update_encoder(self, *_):
        """Picks the encoder from codec, acceleration mode and device, and states clearly what will be used."""
        family, mode = self.vcodec_combo.currentData(), self.accel_combo.currentData()
        hardware = self.hw_encoders(family) if mode != "off" else []
        software = next((c for c in VIDEO_CODECS if c.family == family and not c.vendor
                         and c.encoder in self.encoders.available), None)
        # the device menu always shows what will be used, but can be changed only in manual mode
        devices = ([(f"{self.device_name(c.vendor)} ({c.label})", c.vendor) for c in hardware]
                   or [(f"CPU: {self.cpu}", None)])
        fill_combo(self.device_combo, devices, keep=mode == "manual")
        self.device_combo.setEnabled(mode == "manual" and bool(hardware))
        codec = next((c for c in hardware if c.vendor == self.device_combo.currentData()), software)

        name = FAMILY_NAMES.get(family, "—")
        if codec is None:
            text = f"⚠ No {'software ' if mode == 'off' else ''}encoder for <b>{name}</b> on this PC."
        else:
            if codec.hardware:
                accel, device = "<b>yes</b>", f"<b>{self.device_name(codec.vendor)}</b>"
            else:
                why = "" if mode == "off" else f" (no GPU in this PC can encode {name})"
                accel, device = f"<b>no</b>{why}", f"<b>CPU</b> {self.cpu}"
            rows = (("Codec:", f"<b>{name}</b>"), ("Hardware acceleration:", accel),
                    ("Device:", device), ("Encoder:", codec.label))
            text = "<table cellspacing='0' cellpadding='1'>" + "".join(
                f"<tr><td>{label}&nbsp;&nbsp;</td><td>{value}</td></tr>" for label, value in rows) + "</table>"
        self.encoder_label.setText(text)
        self.set_codec(codec)

    def encoder_summary(self):
        """Plain-text version of the encoder line, for the log."""
        c = self.codec
        device = f"{self.device_name(c.vendor)}, hardware" if c.hardware else "CPU, software"
        return f"{FAMILY_NAMES[c.family]} ({c.label} on {device})"

    def on_format_changed(self, *_):
        out = self.out_edit.text().strip()
        if out and Path(out).suffix.lower() in {"." + f.ext for f in FORMATS}:
            self.out_edit.setText(str(Path(out).with_suffix("." + self.fmt().ext)))
        self.refresh_codecs()
        self.on_settings_changed()   # the format can change the video args (e.g. the HEVC tag)

    def set_codec(self, codec):
        if codec == self.codec:
            return
        self.codec = codec
        if codec:
            best, worst, default = codec.quality
            self.quality_slider.blockSignals(True)
            self.quality_slider.setRange(best, worst)
            self.quality_slider.setValue(default)
            self.quality_slider.blockSignals(False)
            self.speed_combo.setEnabled(codec.speeds is not None)
        self.on_settings_changed()

    def on_acodec_changed(self, *_):
        self.update_audio_widgets()
        self.on_settings_changed()

    def update_audio_widgets(self):
        has_audio = bool(self.media and self.media["has_audio"])
        self.acodec_combo.setEnabled(has_audio)
        self.abitrate_combo.setEnabled(has_audio and self.acodec_combo.currentData() is not None)

    def video_args(self):
        m, codec, fmt = self.media, self.codec, self.fmt()
        filters = []
        target = self.res_combo.currentData()
        if target:
            # scale the short side, so it also works with vertical videos
            filters.append(f"scale=-2:{target}" if m["width"] >= m["height"] else f"scale={target}:-2")
        elif m["width"] % 2 or m["height"] % 2:
            filters.append("scale=trunc(iw/2)*2:trunc(ih/2)*2")  # 4:2:0 video needs even dimensions
        fps = self.fps_combo.currentData()
        if fps:
            filters.append(f"fps={fps}")
        args = ["-map", f"0:{m['video_index']}"]
        if filters:
            args += ["-vf", ",".join(filters)]
        # same keyframe spacing in seconds for every encoder (their defaults go from 0.4 to 10 s)
        gop_frames = max(1, round((fps or m["fps"] or 30) * self.keyint()))
        args += codec.args(self.quality(), self.speed_combo.currentData(), (gop_frames, self.keyint()))
        if codec.family == "hevc" and fmt.ext in ("mp4", "mov"):
            args += ["-tag:v", "hvc1"]   # needed by Apple players
        elif codec.family == "mpeg4" and fmt.ext == "avi":
            args += ["-tag:v", "XVID"]   # recognized by old players
        return args

    def update_quality_label(self):
        if not self.codec:
            self.quality_label.setText("")
            return
        best, worst, _ = self.codec.quality
        q = self.quality()
        self.quality_label.setText(f"{quality_name(q, best, worst).capitalize()} quality "
                                   f"({self.codec.q_name} {q})")

    def on_settings_changed(self, *_):
        if not self._programmatic:
            self.fit = None   # a manual change interrupts the search
        self.update_quality_label()
        if not self.media or self.encoding:
            return
        self.estimator.stop()
        self.est_timer.stop()
        self.fit_btn.setEnabled(False)
        self.start_btn.setEnabled(self.codec is not None)
        if not self.codec:
            self.est_bps = None
            self.size_label.setText("—")
            self.size_detail.setText("⚠ No video encoder available for this format.")
            self.update_limit_status()
            return
        self.size_label.setStyleSheet("color: gray;")
        self.size_detail.setText("Updating estimate…")
        self.est_timer.start()

    # ------------------------------------------------ size estimate
    def run_estimate(self):
        if not self.media or self.encoding or not self.codec:
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
        self.estimator.start(self.media["path"], segments, list(key), self.fmt().ext, self.keyint())

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
        """Searches for the best quality value that stays under the limit."""
        if not self.media or self.est_bps is None:
            return
        target = self.limit_spin.value() * MB * LIMIT_MARGIN
        audio_bytes = self.audio_kbps() * 1000 / 8 * self.media["duration"] * CONTAINER_OVERHEAD
        if audio_bytes >= target:
            QMessageBox.information(self, "Limit too low",
                                    "The audio alone already exceeds the limit: lower or remove it.")
            return
        best, worst, _ = self.codec.quality
        self.fit = {"lo": best, "hi": worst, "best": None, "points": [], "left": 8, "target": target}
        self.fit_btn.setEnabled(False)
        self.fit_step()

    def set_quality(self, value):
        self._programmatic = True
        self.quality_slider.setValue(value)
        self._programmatic = False

    def fit_step(self):
        f = self.fit
        worst = self.codec.quality[1]
        q, size = self.quality(), self.estimated_bytes()
        f["points"].append((q, size))
        if size <= f["target"]:
            f["best"] = q if f["best"] is None else min(f["best"], q)
            f["hi"] = min(f["hi"], q - 1)     # try a higher quality
        else:
            f["lo"] = max(f["lo"], q + 1)     # needs more compression
        f["left"] -= 1

        if f["lo"] > f["hi"] or f["left"] <= 0:
            self.fit = None
            if f["best"] is not None:
                if f["best"] != q:
                    self.set_quality(f["best"])
            elif f["lo"] > worst:
                QMessageBox.information(
                    self, "Limit not reachable",
                    "Even at the lowest quality the file exceeds the limit.\n"
                    "Try reducing the resolution, frame rate or audio.")
            else:
                self.set_quality(min(f["lo"], worst))
            return

        # guess the next value: size drops roughly exponentially as the quality value grows
        slope = -math.log(2) / self.codec.halving
        if len(f["points"]) >= 2:
            (q1, s1), (q2, s2) = f["points"][-2:]
            if q1 != q2 and s1 > 0 and s2 > 0:
                measured = (math.log(s2) - math.log(s1)) / (q2 - q1)
                if measured < 0:
                    slope = measured
        nxt = round(q + (math.log(f["target"]) - math.log(size)) / slope)
        self.set_quality(max(f["lo"], min(f["hi"], nxt)))

    # ------------------------------------------------ output
    def pick_output(self):
        fmt = self.fmt()
        path, _ = QFileDialog.getSaveFileName(self, "Save as", self.out_edit.text(), f"{fmt.label} (*.{fmt.ext})")
        if path:
            if not path.lower().endswith("." + fmt.ext):
                path += "." + fmt.ext
            self.out_edit.setText(path)

    def open_folder(self):
        if self.current_output:
            QDesktopServices.openUrl(QUrl.fromLocalFile(os.path.dirname(self.current_output)))

    # ------------------------------------------------ compression
    def start(self):
        if not self.media or not self.codec:
            return
        fmt = self.fmt()
        inp = self.media["path"]
        out = self.out_edit.text().strip().strip('"')
        if not out:
            QMessageBox.warning(self, "Error", "Choose where to save the file.")
            return
        if Path(out).suffix.lower() != "." + fmt.ext:
            out = str(Path(out).with_suffix("." + fmt.ext))
            self.out_edit.setText(out)
        if os.path.abspath(inp) == os.path.abspath(out):
            QMessageBox.warning(self, "Error", "The output file must be different from the input file.")
            return

        self.est_timer.stop()
        self.estimator.stop()
        acodec, ak = self.audio_codec(), self.audio_kbps()
        # put the seek index at the start of the file, so players can jump anywhere right away
        # (also when the file is streamed or opened over the network)
        index = {"mp4": ["-movflags", "+faststart"], "mov": ["-movflags", "+faststart"],
                 "mkv": ["-cues_to_front", "1"], "webm": ["-cues_to_front", "1"]}.get(fmt.ext, [])
        args = (["-y", "-hide_banner", "-nostats", "-progress", "pipe:1", "-i", inp]
                + self.video_args()
                + (["-map", "0:a:0"] + acodec.args(ak) if acodec else ["-an"])
                + index
                + [out])

        self.cancelled = False
        self.current_output = out
        self.out_buf = ""
        self.stderr_tail = []
        self.progress.setValue(0)
        self.log.clear()
        audio = f"{acodec.label} {ak} kbps" if acodec else "no audio"
        speed = f" · {self.speed_combo.currentText().lower()}" if self.codec.speeds else ""
        self.log.appendPlainText(f"Compressing: {fmt.label} · {self.encoder_summary()} · "
                                 f"{self.codec.q_name} {self.quality()} · {self.res_combo.currentText()} · "
                                 f"{self.fps_combo.currentText()} · {audio}{speed} · "
                                 f"keyframes every {self.keyint()} s")
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
