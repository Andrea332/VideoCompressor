#include "encodercheck.h"

#include <QProcess>

namespace {

QString modeKey(const VideoCodec &codec, RateMode mode)
{
    return codec.encoder + "/" + rateModeName(mode);
}

} // namespace

EncoderCheck::EncoderCheck(const QString &ffmpeg, QObject *parent)
    : QObject(parent), ffmpeg_(ffmpeg)
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
            test(codec, RateMode::Quality);
        else
            available_.insert(codec.encoder);
    }
}

bool EncoderCheck::supports(const VideoCodec &codec, RateMode mode) const
{
    if (!available_.contains(codec.encoder) || !codec.hasMode(mode))
        return false;
    return mode == RateMode::Quality || !codec.hardware() || bitrateModes_.contains(modeKey(codec, mode));
}

// Quality mode first: if the GPU can't encode at all, its bitrate modes are not tried.
void EncoderCheck::test(const VideoCodec &codec, RateMode mode)
{
    auto *proc = new QProcess(this);
    ++pending_;
    connect(proc, &QProcess::finished, this, [this, proc, &codec, mode](int code, QProcess::ExitStatus status) {
        proc->deleteLater();
        if (status == QProcess::NormalExit && code == 0) {
            if (mode == RateMode::Quality) {
                available_.insert(codec.encoder);
                for (RateMode bitrate : {RateMode::Vbr, RateMode::Cbr})
                    if (codec.hasMode(bitrate))
                        test(codec, bitrate);
            } else {
                bitrateModes_.insert(modeKey(codec, mode));
            }
        }
        --pending_;
        emit changed();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            proc->deleteLater();
            --pending_;
            emit changed();
        }
    });
    const RateControl rate{mode, mode == RateMode::Quality ? codec.quality.def : 1000};
    proc->start(ffmpeg_, QStringList{"-hide_banner", "-v", "error", "-f", "lavfi", "-i", "testsrc2=s=320x240:r=30:d=0.2"}
                             + codec.args(rate, "balanced") + QStringList{"-f", "null", "-"});
}
