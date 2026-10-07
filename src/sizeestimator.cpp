#include "sizeestimator.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <cmath>

SizeEstimator::SizeEstimator(const QString &ffmpeg, const QString &ffprobe, QObject *parent)
    : QObject(parent), ffmpeg_(ffmpeg), ffprobe_(ffprobe)
{
}

SizeEstimator::~SizeEstimator()
{
    stop();
}

void SizeEstimator::start(const QString &path, const QList<QPair<double, double>> &segments,
                          const QStringList &videoArgs, const QString &ext, int keyint)
{
    stop();
    tmpdir_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/video_estimate_XXXXXX");
    totalBytes_ = 0;
    totalSecs_ = 0;
    exact_ = segments.size() == 1 && segments[0].first == 0;
    keyint_ = keyint;
    for (int i = 0; i < segments.size(); ++i) {
        const auto [start, length] = segments[i];
        const QString out = tmpdir_->filePath(QString("sample%1.%2").arg(i).arg(ext));
        QStringList args{"-y", "-hide_banner", "-v", "error", "-ss", QString::number(start, 'f', 3),
                         "-t", QString::number(length, 'f', 3), "-i", path};
        args += videoArgs;
        args << "-an" << out;
        queue_.append({args, out, length});
    }
    runNext();
}

void SizeEstimator::stop()
{
    ++gen_;
    queue_.clear();
    if (proc_) {
        QProcess *proc = proc_;
        proc_ = nullptr;
        proc->disconnect(this);
        proc->kill();
        proc->waitForFinished(3000);
        proc->deleteLater();
    }
    tmpdir_.reset();
}

void SizeEstimator::runNext()
{
    if (queue_.isEmpty()) {
        const double bps = totalSecs_ > 0 ? totalBytes_ / totalSecs_ : 0.0;
        tmpdir_.reset();
        emit done(bps, exact_);
        return;
    }
    const Job job = queue_.takeFirst();
    const int gen = gen_;
    auto *proc = new QProcess(this);
    proc_ = proc;
    connect(proc, &QProcess::finished, this, [this, gen, proc, job](int code, QProcess::ExitStatus status) {
        onFinished(gen, proc, job, status == QProcess::NormalExit && code == 0);
    });
    connect(proc, &QProcess::errorOccurred, this, [this, gen, proc, job](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            onFinished(gen, proc, job, false);
    });
    proc->start(ffmpeg_, job.args);
}

void SizeEstimator::onFinished(int gen, QProcess *proc, const Job &job, bool ok)
{
    const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed().right(400);
    if (proc == proc_)
        proc_ = nullptr;
    proc->deleteLater();
    if (gen != gen_)
        return;   // stale estimate, cancelled
    if (!ok || !QFileInfo::exists(job.out)) {
        tmpdir_.reset();
        emit failed(err.isEmpty() ? QStringLiteral("ffmpeg error") : err);
        return;
    }
    double size = double(QFileInfo(job.out).size());
    if (!exact_) {
        // Every sample starts with a keyframe, while the real file has one every keyint_ seconds:
        // remove the keyframes the sample has in excess, otherwise videos with little motion
        // (where keyframes are most of the size) look much bigger than they are
        const double perKeyint = job.seconds / keyint_;
        const double excess = std::ceil(perKeyint - 1e-9) - perKeyint;
        size -= excess * double(firstPacketSize(job.out));
    }
    totalBytes_ += size;
    totalSecs_ += job.seconds;
    runNext();
}

qint64 SizeEstimator::firstPacketSize(const QString &path) const
{
    QProcess proc;
    proc.start(ffprobe_, {"-v", "error", "-select_streams", "v:0", "-read_intervals", "%+#1",
                          "-show_entries", "packet=size", "-of", "csv=p=0", path});
    proc.waitForFinished(30000);
    const QStringList words = QString::fromUtf8(proc.readAllStandardOutput()).split(QRegularExpression("\\s+"),
                                                                                   Qt::SkipEmptyParts);
    return words.isEmpty() ? 0 : QString(words.first()).remove(',').toLongLong();
}
