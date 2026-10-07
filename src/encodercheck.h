#pragma once

#include <QObject>
#include <QSet>
#include <QString>

struct VideoCodec;

// Finds the usable encoders: software ones from "ffmpeg -encoders", GPU ones with a tiny test encode.
class EncoderCheck : public QObject
{
    Q_OBJECT

public:
    explicit EncoderCheck(const QString &ffmpeg, QObject *parent = nullptr);

    const QSet<QString> &available() const { return available_; }
    bool testing() const { return pending_ > 0; }   // GPU tests still running

signals:
    void changed();

private:
    void test(const QString &ffmpeg, const VideoCodec &codec);

    QSet<QString> available_;
    int pending_ = 0;
};
