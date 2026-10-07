#pragma once

#include "codecs.h"

#include <QObject>
#include <QSet>
#include <QString>

// Finds the usable encoders: software ones from "ffmpeg -encoders", GPU ones with a tiny test encode
// (and one more for each bitrate mode, which some GPUs or drivers refuse).
class EncoderCheck : public QObject
{
    Q_OBJECT

public:
    explicit EncoderCheck(const QString &ffmpeg, QObject *parent = nullptr);

    const QSet<QString> &available() const { return available_; }
    // whether the encoder works on this PC with this rate control mode
    bool supports(const VideoCodec &codec, RateMode mode) const;
    bool testing() const { return pending_ > 0; }   // GPU tests still running

signals:
    void changed();

private:
    void test(const VideoCodec &codec, RateMode mode);

    QString ffmpeg_;
    QSet<QString> available_;
    QSet<QString> bitrateModes_;   // "<encoder>/<mode>" that passed the test, for GPU encoders
    int pending_ = 0;
};
