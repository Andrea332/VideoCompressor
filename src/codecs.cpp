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

} // namespace

QStringList VideoCodec::args(int q, const QString &speed, std::optional<std::pair<int, int>> gop) const
{
    QStringList args{"-c:v", encoder};
    args += qArgs(q);
    if (!speeds.isEmpty())
        args += speeds.value(speed);
    if (gop)
        args += gopArgs ? gopArgs(gop->first, gop->second) : QStringList{"-g", QString::number(gop->first)};
    args += extra;
    args << "-pix_fmt" << pixFmt;
    return args;
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

    static const std::vector<VideoCodec> list = {
        {.label = "x264", .encoder = "libx264", .family = "h264", .quality = {18, 45, 28}, .qName = "CRF",
         .qArgs = crf, .speeds = x26x},
        // closed GOP: every keyframe is an IDR, a clean starting point for seeking and editing
        {.label = "x265", .encoder = "libx265", .family = "hevc", .quality = {18, 45, 30}, .qName = "CRF",
         .qArgs = crf, .speeds = x26x, .extra = {"-x265-params", "log-level=error:open-gop=0"}},
        {.label = "SVT-AV1", .encoder = "libsvtav1", .family = "av1", .quality = {20, 63, 35}, .qName = "CRF",
         .qArgs = crf, .speeds = presets("10", "8", "6"), .halving = 8},
        {.label = "libvpx", .encoder = "libvpx-vp9", .family = "vp9", .quality = {15, 63, 33}, .qName = "CRF",
         .qArgs = [](int q) { return crf(q) + QStringList{"-b:v", "0"}; },
         .speeds = presets("5", "4", "2", "-cpu-used"), .halving = 8,
         .extra = {"-row-mt", "1", "-deadline", "good"}},
        {.label = "VVenC", .encoder = "libvvenc", .family = "vvc", .quality = {20, 63, 32}, .qName = "QP",
         .qArgs = [](int q) { return QStringList{"-qp", QString::number(q)}; },
         .speeds = presets("faster", "fast", "medium"), .pixFmt = "yuv420p10le",
         .extra = {"-vvenc-params", "DecodingRefreshType=idr"},
         .gopArgs = [](int, int seconds) { return QStringList{"-period", QString::number(seconds)}; }},  // VVenC ignores -g
        {.label = "Xvid", .encoder = "libxvid", .family = "mpeg4", .quality = {2, 31, 5}, .qName = "Q",
         .qArgs = [](int q) { return QStringList{"-q:v", QString::number(q)}; }, .halving = 5},
        {.label = "NVENC", .encoder = "h264_nvenc", .family = "h264", .quality = {18, 51, 28}, .qName = "CQ",
         .qArgs = nvencCq, .speeds = nvenc, .vendor = "nvidia"},
        {.label = "NVENC", .encoder = "hevc_nvenc", .family = "hevc", .quality = {18, 51, 30}, .qName = "CQ",
         .qArgs = nvencCq, .speeds = nvenc, .vendor = "nvidia"},
        {.label = "NVENC", .encoder = "av1_nvenc", .family = "av1", .quality = {20, 63, 32}, .qName = "CQ",
         .qArgs = nvencCq, .speeds = nvenc, .halving = 8, .vendor = "nvidia"},
        {.label = "AMF", .encoder = "h264_amf", .family = "h264", .quality = {18, 51, 28}, .qName = "QP",
         .qArgs = amfQp({"i", "p", "b"}), .speeds = amf, .vendor = "amd"},
        {.label = "AMF", .encoder = "hevc_amf", .family = "hevc", .quality = {18, 51, 30}, .qName = "QP",
         .qArgs = amfQp({"i", "p"}), .speeds = amf, .vendor = "amd"},
        {.label = "AMF", .encoder = "av1_amf", .family = "av1", .quality = {80, 255, 140}, .qName = "QP",
         .qArgs = amfQp({"i", "p", "b"}), .speeds = amf, .halving = 32, .vendor = "amd"},
        {.label = "Quick Sync", .encoder = "h264_qsv", .family = "h264", .quality = {18, 51, 28}, .qName = "Q",
         .qArgs = qsvIcq, .speeds = qsv, .pixFmt = "nv12", .vendor = "intel"},
        {.label = "Quick Sync", .encoder = "hevc_qsv", .family = "hevc", .quality = {18, 51, 30}, .qName = "Q",
         .qArgs = qsvIcq, .speeds = qsv, .pixFmt = "nv12", .vendor = "intel"},
        {.label = "Quick Sync", .encoder = "av1_qsv", .family = "av1", .quality = {18, 51, 30}, .qName = "Q",
         .qArgs = qsvIcq, .speeds = qsv, .pixFmt = "nv12", .vendor = "intel"},
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
