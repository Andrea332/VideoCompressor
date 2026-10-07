#include "encodercheck.h"

#include "codecs.h"

#include <QProcess>

EncoderCheck::EncoderCheck(const QString &ffmpeg, QObject *parent)
    : QObject(parent)
{
    if (ffmpeg.isEmpty())
        return;
    QProcess proc;
    proc.start(ffmpeg, {"-hide_banner", "-encoders"});
    proc.waitForFinished(30000);
    QSet<QString> listed;
    for (const QString &line : QString::fromUtf8(proc.readAllStandardOutput()).split('\n')) {
        const QStringList parts = line.simplified().split(' ');
        if (parts.size() > 1)
            listed.insert(parts[1]);
    }

    for (const AudioCodec &codec : audioCodecs())
        if (listed.contains(codec.encoder))
            available_.insert(codec.encoder);
    for (const VideoCodec &codec : videoCodecs()) {
        if (!listed.contains(codec.encoder))
            continue;
        if (codec.hardware())
            test(ffmpeg, codec);
        else
            available_.insert(codec.encoder);
    }
}

void EncoderCheck::test(const QString &ffmpeg, const VideoCodec &codec)
{
    auto *proc = new QProcess(this);
    ++pending_;
    const QString encoder = codec.encoder;
    connect(proc, &QProcess::finished, this, [this, proc, encoder](int code, QProcess::ExitStatus status) {
        proc->deleteLater();
        --pending_;
        if (status == QProcess::NormalExit && code == 0)
            available_.insert(encoder);
        emit changed();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            proc->deleteLater();
            --pending_;
            emit changed();
        }
    });
    proc->start(ffmpeg, QStringList{"-hide_banner", "-v", "error", "-f", "lavfi", "-i", "testsrc2=s=320x240:r=30:d=0.2"}
                            + codec.args(codec.quality.def, "balanced") + QStringList{"-f", "null", "-"});
}
