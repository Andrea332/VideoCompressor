#pragma once

#include <QString>

#include <optional>

struct MediaInfo {
    QString path;
    double duration = 0;
    int width = 0;              // as displayed (rotation already applied)
    int height = 0;
    int videoIndex = 0;         // stream index of the main video (cover art is skipped)
    double fps = 0;
    bool hasAudio = false;
    qint64 size = 0;
};

// Prefers the FFmpeg bundled with the app (ffmpeg/ folder next to the executable), otherwise uses PATH.
QString findTool(const QString &name);

// Reads duration, dimensions, fps and audio presence with ffprobe. On failure returns nothing and sets *error.
std::optional<MediaInfo> probe(const QString &ffprobe, const QString &path, QString *error);

double parseRate(const QString &text);
QString fmtTime(double seconds);
QString fmtMb(double bytes);
