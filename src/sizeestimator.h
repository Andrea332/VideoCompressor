#pragma once

#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

#include <memory>

class QProcess;
class QTemporaryDir;

// Encodes a few samples in the background and returns the video's bytes per second.
class SizeEstimator : public QObject
{
    Q_OBJECT

public:
    SizeEstimator(const QString &ffmpeg, const QString &ffprobe, QObject *parent = nullptr);
    ~SizeEstimator() override;

    // segments: (start, length) in seconds; keyint: seconds between keyframes in the real encode
    void start(const QString &path, const QList<QPair<double, double>> &segments, const QStringList &videoArgs,
               const QString &ext, int keyint);
    void stop();
    bool running() const { return proc_ != nullptr; }

signals:
    void done(double bytesPerSecond, bool exact);   // exact: the video was encoded whole
    void failed(const QString &message);

private:
    struct Job {
        QStringList args;
        QString out;
        double seconds;
    };

    void runNext();
    void onFinished(int gen, QProcess *proc, const Job &job, bool ok);
    qint64 firstPacketSize(const QString &path) const;

    QString ffmpeg_;
    QString ffprobe_;
    int gen_ = 0;
    QProcess *proc_ = nullptr;
    QList<Job> queue_;
    std::unique_ptr<QTemporaryDir> tmpdir_;
    double totalBytes_ = 0;
    double totalSecs_ = 0;
    bool exact_ = false;
    int keyint_ = 10;
};
