#include "codecs.h"

namespace {

SpeedArgs presets(const QString &fast, const QString &balanced, const QString &best,
                  const QString &option = QStringLiteral("-preset"))
{
    return {{"fast", {option, fast}}, {"balanced", {option, balanced}}, {"best", {option, best}}};
}

QStringList crf(int q)
{
    return {"-crf", QString::number(q)};
}

QStringList nvencCq(int q)
{
    return {"-rc", "vbr", "-cq", QString::number(q), "-b:v", "0"};
}

QualityArgs amfQp(const QStringList &frameTypes)
{
    return [frameTypes](int q) {
        QStringList args{"-rc", "cqp"};
        for (const QString &type : frameTypes)
            args << "-qp_" + type << QString::number(q);
        return args;
    };
}

QStringList qsvIcq(int q)
{
    return {"-global_quality", QString::number(q)};
}

// VideoToolbox's quality goes from 1 (worst) to 100 (best): the slider keeps "lower value = better quality"
// like every other encoder, and the UI shows VideoToolbox's own number
int videoToolboxQuality(int q)
{
    return 100 - q;
}

QStringList videoToolboxQ(int q)
{
    return {"-q:v", QString::number(videoToolboxQuality(q))};
}

QString kbit(int kbps)
{
    return QString::number(kbps) + "k";
}

// Average bitrate: peaks up to twice the average, within a buffer of one second (a bigger buffer lets
// GPU encoders drift further from the average)
QStringList vbr(int kbps)
{
    return {"-b:v", kbit(kbps), "-maxrate", kbit(2 * kbps), "-bufsize", kbit(kbps)};
}

QStringList cbr(int kbps)
{
    return {"-b:v", kbit(kbps), "-minrate", kbit(kbps), "-maxrate", kbit(kbps), "-bufsize", kbit(kbps)};
}

QStringList averageOnly(int kbps)
{
    return {"-b:v", kbit(kbps)};
}

BitrateArgs with(QStringList (*rate)(int), const QStringList &more)
{
    return [rate, more](int kbps) { return rate(kbps) + more; };
}

// FFmpeg keeps only the last of a repeated option: join the encoder's own parameters given more than once
// (e.g. -x265-params a=1 ... -x265-params b=2 -> -x265-params a=1:b=2)
QStringList mergeEncoderParams(const QStringList &args)
{
    QStringList merged;
    for (qsizetype i = 0; i < args.size(); ++i) {
        if (args[i].startsWith('-') && args[i].endsWith("-params") && i + 1 < args.size()) {
            const qsizetype at = merged.indexOf(args[i]);
            if (at >= 0 && at + 1 < merged.size()) {
                merged[at + 1] += ":" + args[++i];
                continue;
            }
        }
        merged << args[i];
    }
    return merged;
}

} // namespace

bool VideoCodec::hasMode(RateMode mode) const
{
    switch (mode) {
    case RateMode::Quality:
        return true;
    case RateMode::Vbr:
        return bool(vbrArgs);
    case RateMode::Cbr:
        return bool(cbrArgs);
    }
    return false;
}

QStringList VideoCodec::args(const RateControl &rate, const QString &speed,
                             std::optional<std::pair<int, int>> gop) const
{
    QStringList args{"-c:v", encoder};
    switch (rate.mode) {
    case RateMode::Quality:
        args += qArgs(rate.value);
        break;
    case RateMode::Vbr:
        args += vbrArgs(rate.value);
        break;
    case RateMode::Cbr:
        args += cbrArgs(rate.value);
        break;
    }
    if (!speeds.isEmpty())
        args += speeds.value(speed);
    if (gop)
        args += gopArgs ? gopArgs(gop->first, gop->second) : QStringList{"-g", QString::number(gop->first)};
    args += extra;
    args << "-pix_fmt" << pixFmt;
    return mergeEncoderParams(args);
}

QString rateModeName(RateMode mode)
{
    return mode == RateMode::Vbr ? QStringLiteral("VBR") : mode == RateMode::Cbr ? QStringLiteral("CBR") : QString();
}

QStringList AudioCodec::args(int kbps) const
{
    return QStringList{"-c:a", encoder, "-b:a", QString::number(kbps) + "k"} + extra;
}

const std::vector<VideoCodec> &videoCodecs()
{
    static const SpeedArgs x26x = presets("veryfast", "medium", "slow");
    static const SpeedArgs nvenc = presets("p2", "p5", "p7");
    static const SpeedArgs amf = presets("speed", "balanced", "quality", "-quality");
    static const SpeedArgs qsv = presets("veryfast", "medium", "veryslow");
    static const SpeedArgs videoToolbox = {{"fast", {"-prio_speed", "1"}}, {"balanced", {"-prio_speed", "0"}},
                                           {"best", {"-prio_speed", "0"}}};
    // the GPU encoders' own rate control modes; with padding, CBR stays constant even on still images
    static const BitrateArgs nvencVbr = with(vbr, {"-rc", "vbr"});
    static const BitrateArgs nvencCbr = with(cbr, {"-rc", "cbr", "-cbr_padding", "1"});
    static const BitrateArgs amfVbr = with(vbr, {"-rc", "vbr_peak"});
    static const BitrateArgs amfCbr = with(cbr, {"-rc", "cbr", "-filler_data", "1"});
    // VideoToolbox: average bitrate only (its peak limit fails on some Macs); constant bitrate from macOS 13
    static const BitrateArgs videoToolboxCbr = with(averageOnly, {"-constant_bit_rate", "1"});

    static const std::vector<VideoCodec> list = {
        {.label = "x264", .encoder = "libx264", .family = "h264", .quality = {18, 45, 28}, .qName = "CRF",
         .qArgs = crf, .vbrArgs = vbr, .cbrArgs = with(cbr, {"-x264-params", "nal-hrd=cbr"}), .speeds = x26x},
        // closed GOP: every keyframe is an IDR, a clean starting point for seeking and editing
        {.label = "x265", .encoder = "libx265", .family = "hevc", .quality = {18, 45, 30}, .qName = "CRF",
         .qArgs = crf, .vbrArgs = vbr, .cbrArgs = with(cbr, {"-x265-params", "strict-cbr=1"}), .speeds = x26x,
         .extra = {"-x265-params", "log-level=error:open-gop=0"}},
        // SVT-AV1 refuses a peak bitrate in VBR, and has CBR only for low-delay streams
        {.label = "SVT-AV1", .encoder = "libsvtav1", .family = "av1", .quality = {20, 63, 35}, .qName = "CRF",
         .qArgs = crf, .vbrArgs = with(averageOnly, {"-svtav1-params", "overshoot-pct=0"}),
         .speeds = presets("10", "8", "6"), .halving = 8},
        {.label = "libvpx", .encoder = "libvpx-vp9", .family = "vp9", .quality = {15, 63, 33}, .qName = "CRF",
         .qArgs = [](int q) { return crf(q) + QStringList{"-b:v", "0"}; }, .vbrArgs = vbr, .cbrArgs = cbr,
         .speeds = presets("5", "4", "2", "-cpu-used"), .halving = 8,
         .extra = {"-row-mt", "1", "-deadline", "good"}},
        {.label = "VVenC", .encoder = "libvvenc", .family = "vvc", .quality = {20, 63, 32}, .qName = "QP",
         .qArgs = [](int q) { return QStringList{"-qp", QString::number(q)}; }, .vbrArgs = averageOnly,
         .speeds = presets("faster", "fast", "medium"), .pixFmt = "yuv420p10le",
         .extra = {"-vvenc-params", "DecodingRefreshType=idr"},
         .gopArgs = [](int, int seconds) { return QStringList{"-period", QString::number(seconds)}; }},  // VVenC ignores -g
        {.label = "Xvid", .encoder = "libxvid", .family = "mpeg4", .quality = {2, 31, 5}, .qName = "Q",
         .qArgs = [](int q) { return QStringList{"-q:v", QString::number(q)}; }, .vbrArgs = averageOnly,
         .halving = 5},
        {.label = "NVENC", .encoder = "h264_nvenc", .family = "h264", .quality = {18, 51, 28}, .qName = "CQ",
         .qArgs = nvencCq, .vbrArgs = nvencVbr, .cbrArgs = nvencCbr, .speeds = nvenc, .vendor = "nvidia"},
        {.label = "NVENC", .encoder = "hevc_nvenc", .family = "hevc", .quality = {18, 51, 30}, .qName = "CQ",
         .qArgs = nvencCq, .vbrArgs = nvencVbr, .cbrArgs = nvencCbr, .speeds = nvenc, .vendor = "nvidia"},
        {.label = "NVENC", .encoder = "av1_nvenc", .family = "av1", .quality = {20, 63, 32}, .qName = "CQ",
         .qArgs = nvencCq, .vbrArgs = nvencVbr, .cbrArgs = nvencCbr, .speeds = nvenc, .halving = 8,
         .vendor = "nvidia"},
        {.label = "AMF", .encoder = "h264_amf", .family = "h264", .quality = {18, 51, 28}, .qName = "QP",
         .qArgs = amfQp({"i", "p", "b"}), .vbrArgs = amfVbr, .cbrArgs = amfCbr, .speeds = amf, .vendor = "amd"},
        {.label = "AMF", .encoder = "hevc_amf", .family = "hevc", .quality = {18, 51, 30}, .qName = "QP",
         .qArgs = amfQp({"i", "p"}), .vbrArgs = amfVbr, .cbrArgs = amfCbr, .speeds = amf, .vendor = "amd"},
        {.label = "AMF", .encoder = "av1_amf", .family = "av1", .quality = {80, 255, 140}, .qName = "QP",
         .qArgs = amfQp({"i", "p", "b"}), .vbrArgs = amfVbr, .cbrArgs = amfCbr, .speeds = amf, .halving = 32,
         .vendor = "amd"},
        {.label = "Quick Sync", .encoder = "h264_qsv", .family = "h264", .quality = {18, 51, 28}, .qName = "Q",
         .qArgs = qsvIcq, .vbrArgs = vbr, .cbrArgs = cbr, .speeds = qsv, .pixFmt = "nv12", .vendor = "intel"},
        {.label = "Quick Sync", .encoder = "hevc_qsv", .family = "hevc", .quality = {18, 51, 30}, .qName = "Q",
         .qArgs = qsvIcq, .vbrArgs = vbr, .cbrArgs = cbr, .speeds = qsv, .pixFmt = "nv12", .vendor = "intel"},
        {.label = "Quick Sync", .encoder = "av1_qsv", .family = "av1", .quality = {18, 51, 30}, .qName = "Q",
         .qArgs = qsvIcq, .vbrArgs = vbr, .cbrArgs = cbr, .speeds = qsv, .pixFmt = "nv12", .vendor = "intel"},
        {.label = "VideoToolbox", .encoder = "h264_videotoolbox", .family = "h264", .quality = {20, 85, 45},
         .qName = "Q", .qArgs = videoToolboxQ, .vbrArgs = averageOnly, .cbrArgs = videoToolboxCbr,
         .speeds = videoToolbox, .halving = 10, .vendor = "apple", .qShown = videoToolboxQuality},
        {.label = "VideoToolbox", .encoder = "hevc_videotoolbox", .family = "hevc", .quality = {20, 85, 45},
         .qName = "Q", .qArgs = videoToolboxQ, .vbrArgs = averageOnly, .cbrArgs = videoToolboxCbr,
         .speeds = videoToolbox, .halving = 10, .vendor = "apple", .qShown = videoToolboxQuality},
    };
    return list;
}

const std::vector<AudioCodec> &audioCodecs()
{
    static const std::vector<AudioCodec> list = {
        {"AAC", "aac", "aac", {}},
        // libopus only accepts the standard channel layouts: e.g. 5.1(side) becomes 5.1
        {"Opus", "libopus", "opus", {"-af", "aformat=channel_layouts=mono|stereo|3.0|quad|5.0|5.1|6.1|7.1"}},
        {"MP3", "libmp3lame", "mp3", {}},
        {"Vorbis", "libvorbis", "vorbis", {}},
    };
    return list;
}

const std::vector<Format> &formats()
{
    static const std::vector<Format> list = {
        {"MP4", "mp4", {"h264", "hevc", "av1", "vp9", "vvc", "mpeg4"}, {"aac", "opus", "mp3"}},
        {"MKV", "mkv", {"h264", "hevc", "av1", "vp9", "vvc", "mpeg4"}, {"aac", "opus", "mp3", "vorbis"}},
        {"WebM", "webm", {"av1", "vp9"}, {"opus", "vorbis"}},
        {"MOV", "mov", {"h264", "hevc"}, {"aac", "mp3"}},
        {"AVI", "avi", {"mpeg4"}, {"mp3"}},
    };
    return list;
}

const std::vector<Family> &families()
{
    static const std::vector<Family> list = {
        {"h264", "H.264 / AVC", ""},
        {"hevc", "H.265 / HEVC", ""},
        {"av1", "AV1", ""},
        {"vp9", "VP9", ""},
        {"vvc", "H.266 / VVC", "very slow"},
        {"mpeg4", "MPEG-4 Part 2 / Xvid", "old devices"},
    };
    return list;
}

const std::vector<Vendor> &vendors()
{
    static const std::vector<Vendor> list = {
        {"nvidia", "NVIDIA GPU", "NVENC", "10DE"},
        {"apple", "Apple GPU", "VideoToolbox", ""},
        {"intel", "Intel GPU", "Quick Sync", "8086"},
        {"amd", "AMD GPU", "AMF", "1002"},
    };
    return list;
}

const Format &formatByExt(const QString &ext)
{
    for (const Format &f : formats())
        if (f.ext == ext)
            return f;
    return formats().front();
}

const AudioCodec *audioCodecByName(const QString &name)
{
    for (const AudioCodec &a : audioCodecs())
        if (a.name == name)
            return &a;
    return nullptr;
}

QString familyName(const QString &key)
{
    for (const Family &f : families())
        if (f.key == key)
            return f.name;
    return QStringLiteral("—");
}

const Vendor *vendorByKey(const QString &key)
{
    for (const Vendor &v : vendors())
        if (v.key == key)
            return &v;
    return nullptr;
}

int vendorRank(const QString &key)
{
    const auto &list = vendors();
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].key == key)
            return int(i);
    return int(list.size());
}

QString qualityName(int q, const QualityScale &scale)
{
    const double frac = double(q - scale.best) / (scale.worst - scale.best);
    static const std::pair<double, const char *> steps[] = {
        {0.08, "very high"}, {0.23, "high"}, {0.38, "good"}, {0.53, "medium"}, {0.71, "low"}};
    for (const auto &[limit, name] : steps)
        if (frac <= limit)
            return QString::fromLatin1(name);
    return QStringLiteral("very low");
}

const std::vector<SpeedLevel> &speedLevels()
{
    static const std::vector<SpeedLevel> list = {
        {"Balanced", "balanced"}, {"Fast", "fast"}, {"Best quality (slow)", "best"}};
    return list;
}

const std::vector<KeyframeInterval> &keyframeIntervals()
{
    static const std::vector<KeyframeInterval> list = {
        {"Every 10 s (smallest file)", 10}, {"Every 5 s", 5}, {"Every 2 s (editing, streaming)", 2}, {"Every 1 s", 1}};
    return list;
}

const std::vector<int> &audioBitrates()
{
    static const std::vector<int> list = {64, 96, 128, 160, 192};
    return list;
}

const std::vector<int> &resolutionSteps()
{
    static const std::vector<int> list = {1080, 720, 540, 480, 360};
    return list;
}

const std::vector<int> &fpsSteps()
{
    static const std::vector<int> list = {30, 24, 15};
    return list;
}
