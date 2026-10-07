#include "media.h"

#include "codecs.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

#include <cmath>

QString findTool(const QString &name)
{
#ifdef Q_OS_WIN
    const QString exe = name + ".exe";
#else
    const QString exe = name;
#endif
    // ffmpeg/<exe> on Windows and macOS; ffmpeg/bin/<exe> next to ffmpeg/lib/ on Linux
    for (const QString &dir : {"/ffmpeg/", "/ffmpeg/bin/"}) {
        const QString bundled = QCoreApplication::applicationDirPath() + dir + exe;
        if (QFileInfo(bundled).isFile())
            return bundled;
    }
    return QStandardPaths::findExecutable(name);
}

double parseRate(const QString &text)
{
    const QStringList parts = text.split('/');
    if (parts.size() != 2)
        return 0.0;
    bool okNum = false, okDen = false;
    const double num = parts[0].toDouble(&okNum);
    const double den = parts[1].toDouble(&okDen);
    return okNum && okDen && den != 0 ? num / den : 0.0;
}

std::optional<MediaInfo> probe(const QString &ffprobe, const QString &path, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return std::optional<MediaInfo>();
    };
    const QFileInfo file(path);
    if (!file.isFile())
        return fail("file not found");

    QProcess proc;
    proc.start(ffprobe, {"-v", "error", "-show_entries",
                         "stream=index,codec_type,width,height,avg_frame_rate,r_frame_rate,duration:"
                         "stream_disposition=attached_pic:stream_side_data=rotation:format=duration",
                         "-of", "json", path});
    if (!proc.waitForFinished(60000))
        return fail("ffprobe did not respond");
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        const QStringList lines = QString::fromUtf8(proc.readAllStandardError()).trimmed().split('\n');
        return fail(lines.last().trimmed().isEmpty() ? QStringLiteral("unrecognized format") : lines.last().trimmed());
    }

    const QJsonObject info = QJsonDocument::fromJson(proc.readAllStandardOutput()).object();
    const QJsonArray streams = info.value("streams").toArray();
    QJsonObject video;
    bool hasAudio = false;
    for (const QJsonValue &value : streams) {
        const QJsonObject s = value.toObject();
        const QString type = s.value("codec_type").toString();
        hasAudio |= type == "audio";
        // cover art is reported as a one-frame video stream: skip it
        if (type == "video" && video.isEmpty() && !s.value("disposition").toObject().value("attached_pic").toInt())
            video = s;
    }
    if (video.isEmpty())
        return fail("no video stream found");

    QString duration = info.value("format").toObject().value("duration").toString();
    if (duration.isEmpty())
        duration = video.value("duration").toString();
    bool ok = false;
    MediaInfo m;
    m.duration = duration.toDouble(&ok);
    if (!ok)
        return fail("unknown duration");
    if (m.duration <= 0)
        return fail("invalid duration");

    m.path = path;
    m.width = video.value("width").toInt();
    m.height = video.value("height").toInt();
    // phone videos are often stored landscape with a rotation flag: use the size as displayed
    for (const QJsonValue &sideData : video.value("side_data_list").toArray()) {
        const QJsonObject d = sideData.toObject();
        if (d.contains("rotation")) {
            if (std::lround(std::abs(d.value("rotation").toDouble())) % 180 == 90)
                std::swap(m.width, m.height);
            break;
        }
    }
    m.videoIndex = video.value("index").toInt();
    m.fps = parseRate(video.value("avg_frame_rate").toString());
    if (m.fps == 0)
        m.fps = parseRate(video.value("r_frame_rate").toString());
    m.hasAudio = hasAudio;
    m.size = file.size();
    return m;
}

QString fmtTime(double seconds)
{
    const long long total = std::llround(seconds);
    const long long h = total / 3600, m = total % 3600 / 60, s = total % 60;
    return h ? QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'))
             : QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

QString fmtMb(double bytes)
{
    const double mb = bytes / MB;
    return QString::number(mb, 'f', mb < 10 ? 2 : 1) + " MB";
}
