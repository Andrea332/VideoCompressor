#pragma once

#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

// Output formats, video and audio codecs, and the FFmpeg arguments for each encoder.

inline constexpr int SAMPLE_COUNT = 4;               // samples used for the estimate
inline constexpr double SAMPLE_LEN = 4.0;            // seconds per sample
inline constexpr double WHOLE_IF_SHORTER = 20.0;     // shorter videos are encoded whole (exact estimate)
inline constexpr double CONTAINER_OVERHEAD = 1.01;   // ~1% for the container
inline constexpr double LIMIT_MARGIN = 0.94;         // safety margin for "Fit to limit"
inline constexpr double GPU_VBR_LIMIT_MARGIN = 0.90; // GPU encoders follow an average bitrate less closely
inline constexpr double MB = 1024.0 * 1024.0;

// How the encoder decides how many bits each part of the video gets
enum class RateMode {
    Quality,    // constant quality: as many bits as each scene needs (CRF, CQ, QP...)
    Vbr,        // average bitrate, more for complex scenes and less for simple ones
    Cbr,        // the same bitrate all the time
};

struct RateControl {
    RateMode mode = RateMode::Quality;
    int value = 0;              // quality on the encoder's scale, or kbps for VBR and CBR
};

using QualityArgs = std::function<QStringList(int quality)>;
using BitrateArgs = std::function<QStringList(int kbps)>;
using GopArgs = std::function<QStringList(int frames, int seconds)>;
using SpeedArgs = QMap<QString, QStringList>;        // "fast"/"balanced"/"best" -> ffmpeg args

struct QualityScale {
    int best;      // lower = better on every encoder's scale
    int worst;
    int def;
};

struct VideoCodec {
    QString label;              // name of the encoder shown in the UI, e.g. "x264", "NVENC"
    QString encoder;            // FFmpeg encoder, e.g. "libx264"
    QString family;             // h264, hevc, av1, vp9, vvc or mpeg4
    QualityScale quality;
    QString qName;              // name of the quality parameter shown in the UI
    QualityArgs qArgs;
    BitrateArgs vbrArgs;        // empty = the encoder has no variable bitrate mode
    BitrateArgs cbrArgs;        // empty = the encoder has no constant bitrate mode
    SpeedArgs speeds;           // empty = no presets
    QString pixFmt = "yuv420p";
    double halving = 6;         // quality steps that roughly halve the size (first guess for "Fit to limit")
    QStringList extra;
    QString vendor;             // GPU encoder of this vendor (see vendors()), empty = software
    GopArgs gopArgs;            // keyframe interval, empty = "-g <frames>"
    std::function<int(int)> qShown;   // value shown in the UI for an encoder whose own scale is reversed

    bool hardware() const { return !vendor.isEmpty(); }
    int shownQuality(int value) const { return qShown ? qShown(value) : value; }
    // whether the encoder has this mode (GPU encoders may still refuse it, see EncoderCheck)
    bool hasMode(RateMode mode) const;
    // gop: (frames, seconds) between keyframes, empty = encoder default
    QStringList args(const RateControl &rate, const QString &speed,
                     std::optional<std::pair<int, int>> gop = {}) const;
};

QString rateModeName(RateMode mode);   // "VBR", "CBR" or "" for constant quality

struct AudioCodec {
    QString label;
    QString encoder;
    QString name;
    QStringList extra;

    QStringList args(int kbps) const;
};

struct Format {
    QString label;
    QString ext;
    QSet<QString> video;        // accepted video codec families
    QSet<QString> audio;        // accepted audio codec names
};

struct Family {
    QString key;
    QString name;
    QString note;
};

struct Vendor {
    QString key;
    QString genericName;        // used when the GPU name is unknown
    QString technology;         // e.g. "NVENC"
    QString pciId;              // PCI vendor id, e.g. "10DE"
};

const std::vector<VideoCodec> &videoCodecs();
const std::vector<AudioCodec> &audioCodecs();
const std::vector<Format> &formats();
const std::vector<Family> &families();      // video codecs shown in the menu
const std::vector<Vendor> &vendors();       // GPU vendors, in order of preference for the automatic choice

const Format &formatByExt(const QString &ext);
const AudioCodec *audioCodecByName(const QString &name);
QString familyName(const QString &key);
const Vendor *vendorByKey(const QString &key);
int vendorRank(const QString &key);

// "very high" ... "very low", from the position of q on the encoder's scale
QString qualityName(int q, const QualityScale &scale);

struct SpeedLevel {
    QString label;
    QString key;
};
const std::vector<SpeedLevel> &speedLevels();

struct KeyframeInterval {
    QString label;
    int seconds;
};
const std::vector<KeyframeInterval> &keyframeIntervals();

const std::vector<int> &audioBitrates();
inline constexpr int DEFAULT_AUDIO_KBPS = 96;
inline constexpr int MIN_VIDEO_KBPS = 50;            // range of the video bitrate field (VBR and CBR)
inline constexpr int MAX_VIDEO_KBPS = 200000;
inline constexpr int DEFAULT_VIDEO_KBPS = 2500;
const std::vector<int> &resolutionSteps();
const std::vector<int> &fpsSteps();
