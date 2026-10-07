// End-to-end tests: they drive the real window (offscreen) and check the files FFmpeg produces.
// Test videos are generated with FFmpeg at startup; GPU encoders are tested only if this PC has them.

#include "codecs.h"
#include "encodercheck.h"
#include "mainwindow.h"
#include "media.h"
#include "sizeestimator.h"
#include "updatechecker.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <memory>

class TestVideoCompressor : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void probesTrickyInputs();
    void showsVersionAndSourcePreview();
    void offersOnlyCompatibleCodecs();
    void statesEncoderClearly();
    void encodesEveryFormatCodecAndDevice();
    void scalesPortraitPhoneVideos();
    void usesVideoNotCoverArt();
    void placesKeyframesWithClosedGop();
    void writesSeekIndexAtStart();
    void estimatesAccurately_data();
    void estimatesAccurately();
    void fitsToLimit();
    void notifiesAboutUpdates();
    void survivesClosingWhileBusy();
    void rendersWindow();

private:
    bool usable(const QString &family) const;
    QString path(const QString &name) const { return dir_.filePath(name); }
    QByteArray run(const QString &program, const QStringList &args, int timeoutMs = 300000);
    void select(QComboBox *combo, const QVariant &data);
    bool settled() const;
    QString compress(const QString &input, const QString &format, const QString &family, const QString &mode,
                     const QString &vendor = {}, const QString &audio = {}, int keyint = 10, int resolution = 0);
    QJsonArray streams(const QString &file);
    QList<double> keyframeTimes(const QString &file);
    QList<int> nalTypes(const QString &file);
    QStringList hardwareVendors(const QString &family) const;

    QTemporaryDir dir_;
    QString ffmpeg_;
    QString ffprobe_;
    std::unique_ptr<MainWindow> win_;
    QStringList messages_;
};

// ------------------------------------------------ helpers
QByteArray TestVideoCompressor::run(const QString &program, const QStringList &args, int timeoutMs)
{
    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForFinished(timeoutMs) || proc.exitCode() != 0)
        qWarning().noquote() << program << args.join(' ') << "failed:" << proc.readAllStandardError().right(500);
    return proc.readAllStandardOutput() + proc.readAllStandardError();
}

void TestVideoCompressor::select(QComboBox *combo, const QVariant &data)
{
    const int index = combo->findData(data);
    QVERIFY2(index >= 0, qPrintable(QString("%1 not offered").arg(data.toString())));
    combo->setCurrentIndex(index);
}

bool TestVideoCompressor::settled() const
{
    const MainWindow &w = *win_;
    if (w.estTimer_->isActive() || w.estimator_->running())
        return false;
    return w.sizeDetail_->text().startsWith("⚠") || w.estCache_.contains(w.videoArgs().join(QChar(0x1F)));
}

QString TestVideoCompressor::compress(const QString &input, const QString &format, const QString &family,
                                      const QString &mode, const QString &vendor, const QString &audio, int keyint,
                                      int resolution)
{
    MainWindow &w = *win_;
    w.setInput(input);
    select(w.formatCombo_, format);
    select(w.vcodecCombo_, family);
    select(w.accelCombo_, mode);
    if (mode == "manual")
        select(w.deviceCombo_, vendor);
    select(w.acodecCombo_, audio);
    select(w.keyintCombo_, keyint);
    select(w.resCombo_, resolution);
    const QString out = path("out." + format);
    QFile::remove(out);
    w.outEdit_->setText(out);
    w.start();
    if (!QTest::qWaitFor([&] { return !w.encoding_; }, 600000))
        return QString();
    return w.log_->toPlainText().contains("Done!") ? out : QString();
}

QJsonArray TestVideoCompressor::streams(const QString &file)
{
    const QByteArray json = run(ffprobe_, {"-v", "error", "-show_entries",
                                           "stream=codec_type,codec_name,width,height,channels:stream_tags=encoder",
                                           "-of", "json", file});
    return QJsonDocument::fromJson(json).object().value("streams").toArray();
}

QList<double> TestVideoCompressor::keyframeTimes(const QString &file)
{
    QList<double> times;
    const QString csv = run(ffprobe_, {"-v", "error", "-select_streams", "v:0", "-show_entries",
                                       "packet=pts_time,flags", "-of", "csv=p=0", file});
    for (const QString &line : csv.split('\n', Qt::SkipEmptyParts)) {
        const QStringList parts = line.trimmed().split(',');
        if (parts.size() >= 2 && parts[1].contains('K'))
            times << parts[0].toDouble();
    }
    return times;
}

QList<int> TestVideoCompressor::nalTypes(const QString &file)
{
    QList<int> types;
    const QString trace = run(ffmpeg_, {"-v", "trace", "-i", file, "-c", "copy", "-bsf:v", "trace_headers",
                                        "-f", "null", "-"});
    static const QRegularExpression re("nal_unit_type\\s+\\d+ = (\\d+)");
    for (auto it = re.globalMatch(trace); it.hasNext();)
        types << it.next().captured(1).toInt();
    return types;
}

// whether this PC's FFmpeg has an encoder for the codec (the FFmpeg builds differ a little between platforms)
bool TestVideoCompressor::usable(const QString &family) const
{
    return std::any_of(videoCodecs().begin(), videoCodecs().end(), [&](const VideoCodec &c) {
        return c.family == family && win_->encoders_->available().contains(c.encoder);
    });
}

QStringList TestVideoCompressor::hardwareVendors(const QString &family) const
{
    QStringList list;
    for (const VideoCodec *c : win_->hwEncoders(family))
        list << c->vendor;
    return list;
}

QString plainText(const QString &html)
{
    QTextDocument doc;
    doc.setHtml(html);
    return doc.toPlainText().simplified();
}

// ------------------------------------------------ setup
void TestVideoCompressor::initTestCase()
{
    QVERIFY(dir_.isValid());
    ffmpeg_ = findTool("ffmpeg");
    ffprobe_ = findTool("ffprobe");
    QVERIFY2(!ffmpeg_.isEmpty() && !ffprobe_.isEmpty(), "FFmpeg is needed to run the tests");

    const QStringList common{"-y", "-v", "error"};
    run(ffmpeg_, common + QStringList{"-f", "lavfi", "-i", "testsrc2=s=1280x720:r=30:d=8", "-f", "lavfi", "-i",
                                      "sine=f=440:d=8", "-c:v", "libx264", "-crf", "18", "-c:a", "aac", "-shortest",
                                      path("input.mp4")});
    run(ffmpeg_, common + QStringList{"-f", "lavfi", "-i", "testsrc2=s=1280x720:r=30:d=40", "-f", "lavfi", "-i",
                                      "sine=f=440:d=40", "-c:v", "libx264", "-crf", "18", "-c:a", "aac", "-shortest",
                                      path("long.mp4")});
    run(ffmpeg_, common + QStringList{"-t", "20", "-i", path("long.mp4"), "-c", "copy", path("20s.mp4")});
    // phone video: stored landscape with a 90 degree rotation flag
    run(ffmpeg_, common + QStringList{"-display_rotation", "90", "-i", path("input.mp4"), "-c", "copy",
                                      path("rotated.mp4")});
    run(ffmpeg_, common + QStringList{"-f", "lavfi", "-i", "color=red:s=300x300", "-frames:v", "1", path("cover.png")});
    run(ffmpeg_, common + QStringList{"-i", path("cover.png"), "-i", path("input.mp4"), "-map", "0", "-map", "1",
                                      "-c", "copy", "-disposition:v:0", "attached_pic", path("cover.mp4")});
    // 5.1(side) audio and subtitles: the cases that broke Opus and subtitle auto-mapping
    QFile srt(path("subs.srt"));
    QVERIFY(srt.open(QIODevice::WriteOnly));
    srt.write("1\n00:00:01,000 --> 00:00:02,000\nHello\n");
    srt.close();
    run(ffmpeg_, common + QStringList{"-f", "lavfi", "-i", "testsrc2=s=640x360:r=25:d=3", "-f", "lavfi", "-i",
                                      "sine=f=440:d=3", "-i", path("subs.srt"), "-filter_complex",
                                      "[1:a]aformat=channel_layouts=5.1(side)[a]", "-map", "0:v", "-map", "[a]",
                                      "-map", "2:s", "-c:v", "libx264", "-crf", "20", "-c:a", "ac3", "-c:s", "srt",
                                      path("stress.mkv")});
    // little motion, like a screen recording: keyframes are most of the size
    run(ffmpeg_, common + QStringList{"-f", "lavfi", "-i", "testsrc2=s=1920x1080:r=30:d=1", "-f", "lavfi", "-i",
                                      "color=red:s=60x60:r=30:d=30", "-filter_complex",
                                      "[0:v]loop=loop=-1:size=1,trim=duration=30,setpts=N/30/TB[bg];"
                                      "[bg][1:v]overlay=x='mod(t*80,1860)':y=500",
                                      "-c:v", "libx264", "-crf", "12", path("static.mp4")});
    for (const QString &name : {"input.mp4", "long.mp4", "20s.mp4", "rotated.mp4", "cover.mp4", "stress.mkv", "static.mp4"})
        QVERIFY2(QFile::exists(path(name)), qPrintable(name + " was not created"));

    win_ = std::make_unique<MainWindow>();
    win_->showMessage = [this](QMessageBox::Icon, const QString &title, const QString &) { messages_ << title; };
    win_->show();
    QVERIFY(QTest::qWaitFor([this] { return !win_->encoders_->testing(); }, 30000));
    qInfo().noquote() << "Encoders:" << QStringList(win_->encoders_->available().values()).join(' ');
    qInfo().noquote() << "GPUs:" << QStringList(win_->gpus_.values()).join(", ") << "| CPU:" << win_->cpu_;
}

// ------------------------------------------------ tests
void TestVideoCompressor::probesTrickyInputs()
{
    QString error;
    const auto rotated = probe(ffprobe_, path("rotated.mp4"), &error);
    QVERIFY2(rotated, qPrintable(error));
    QCOMPARE(rotated->width, 720);    // as displayed
    QCOMPARE(rotated->height, 1280);

    const auto cover = probe(ffprobe_, path("cover.mp4"), &error);
    QVERIFY(cover);
    QCOMPARE(cover->width, 1280);     // the video, not the 300x300 cover
    QCOMPARE(cover->height, 720);

    const auto stress = probe(ffprobe_, path("stress.mkv"), &error);
    QVERIFY(stress && stress->hasAudio);
    QVERIFY(std::abs(stress->duration - 3.0) < 0.1);

    QVERIFY(!probe(ffprobe_, path("missing.mp4"), &error));
    QCOMPARE(error, QString("file not found"));
}

void TestVideoCompressor::showsVersionAndSourcePreview()
{
    MainWindow &w = *win_;
    QCOMPARE(w.windowTitle(), QString("Video Compressor 9.9.9"));   // version set in main() below

    const auto previewSize = [&](const QString &name) {
        w.setInput(path(name));
        if (!QTest::qWaitFor([&] { return w.preview_->isVisible(); }, 30000))
            return QSize();
        return w.preview_->pixmap().deviceIndependentSize().toSize();
    };
    const QSize landscape = previewSize("input.mp4");   // 16:9: fills the 192x108 box
    QVERIFY2(landscape.width() == 192 && std::abs(landscape.height() - 108) <= 1,
             qPrintable(QString("%1x%2").arg(landscape.width()).arg(landscape.height())));
    const QSize portrait = previewSize("rotated.mp4");  // phone video: stays vertical
    QVERIFY2(portrait.height() == 108 && portrait.width() < 70,
             qPrintable(QString("%1x%2").arg(portrait.width()).arg(portrait.height())));

    w.setInput(path("missing.mp4"));
    QVERIFY(!w.preview_->isVisible());
}

void TestVideoCompressor::offersOnlyCompatibleCodecs()
{
    MainWindow &w = *win_;
    QMap<QString, QStringList> expected;   // the families each format accepts, if this FFmpeg can encode them
    const QMap<QString, QStringList> accepted = {
        {"mp4", {"h264", "hevc", "av1", "vp9", "vvc", "mpeg4"}}, {"mkv", {"h264", "hevc", "av1", "vp9", "vvc", "mpeg4"}},
        {"webm", {"av1", "vp9"}}, {"mov", {"h264", "hevc"}}, {"avi", {"mpeg4"}}};
    for (auto it = accepted.begin(); it != accepted.end(); ++it)
        for (const QString &family : it.value())
            if (usable(family))
                expected[it.key()] << family;
    QVERIFY(usable("h264"));   // x264 is in every FFmpeg build we ship
    for (auto it = expected.begin(); it != expected.end(); ++it) {
        select(w.formatCombo_, it.key());
        QStringList offered;
        for (int i = 0; i < w.vcodecCombo_->count(); ++i) {
            const QString family = w.vcodecCombo_->itemData(i).toString();
            offered << family;
            // the bolt icon appears exactly when a GPU of this PC can encode the codec
            const bool bolt = w.vcodecCombo_->itemIcon(i).cacheKey() == w.hwIcon_.cacheKey();
            QCOMPARE(bolt, !hardwareVendors(family).isEmpty());
        }
        QCOMPARE(offered, it.value());
    }
    select(w.formatCombo_, "mp4");
}

void TestVideoCompressor::statesEncoderClearly()
{
    MainWindow &w = *win_;
    w.setInput(path("input.mp4"));
    select(w.formatCombo_, "mp4");
    select(w.vcodecCombo_, "h264");
    select(w.accelCombo_, "off");
    QString text = plainText(w.encoderLabel_->text());
    QVERIFY2(text.contains("Codec: H.264 / AVC") && text.contains("Hardware acceleration: no")
                 && text.contains("Device: CPU " + w.cpu_) && text.contains("Encoder: x264"),
             qPrintable(text));
    QCOMPARE(w.codec_->encoder, QString("libx264"));
    QVERIFY(!w.deviceCombo_->isEnabled());

    select(w.accelCombo_, "auto");
    const QStringList gpus = hardwareVendors("h264");
    text = plainText(w.encoderLabel_->text());
    if (gpus.isEmpty()) {
        QVERIFY2(text.contains("no (no GPU in this PC can encode H.264 / AVC)"), qPrintable(text));
    } else {
        QVERIFY2(text.contains("Hardware acceleration: yes") && text.contains(w.deviceName(gpus.first())),
                 qPrintable(text));
        QCOMPARE(w.codec_->vendor, gpus.first());   // the automatic choice is the preferred vendor
        select(w.accelCombo_, "manual");
        QVERIFY(w.deviceCombo_->isEnabled());
        select(w.deviceCombo_, gpus.last());
        QCOMPARE(w.codec_->vendor, gpus.last());
    }

    if (usable("vp9")) {
        select(w.formatCombo_, "webm");
        select(w.vcodecCombo_, "vp9");
        select(w.accelCombo_, "auto");
        text = plainText(w.encoderLabel_->text());
        QVERIFY2(text.contains("no (no GPU in this PC can encode VP9)") && text.contains("Encoder: libvpx"),
                 qPrintable(text));
    }
    select(w.formatCombo_, "mp4");
}

void TestVideoCompressor::encodesEveryFormatCodecAndDevice()
{
    MainWindow &w = *win_;
    static const QMap<QString, QString> streamCodec = {{"h264", "h264"}, {"hevc", "hevc"}, {"av1", "av1"},
                                                       {"vp9", "vp9"}, {"vvc", "vvc"}, {"mpeg4", "mpeg4"}};
    int encodes = 0;
    for (const Format &f : formats()) {
        select(w.formatCombo_, f.ext);
        QStringList families, audios;
        for (int i = 0; i < w.vcodecCombo_->count(); ++i)
            families << w.vcodecCombo_->itemData(i).toString();
        for (int i = 0; i < w.acodecCombo_->count(); ++i)
            audios << w.acodecCombo_->itemData(i).toString();   // the last one is "" (no audio)
        if (families.isEmpty())
            continue;   // e.g. AVI without an Xvid encoder in this FFmpeg

        // every codec on every device, then every audio codec
        QList<std::tuple<QString, QString, QString, QString>> cases;
        for (const QString &family : families) {
            cases.append({family, "off", "", audios.first()});
            for (const QString &vendor : hardwareVendors(family))
                cases.append({family, "manual", vendor, audios.first()});
        }
        for (const QString &audio : audios.mid(1))
            cases.append({families.first(), "off", "", audio});

        for (const auto &[family, mode, vendor, audio] : cases) {
            const QString out = compress(path("stress.mkv"), f.ext, family, mode, vendor, audio);
            const QString what = QString("%1 %2 %3 %4").arg(f.ext, family, vendor.isEmpty() ? "CPU" : vendor,
                                                            audio.isEmpty() ? "no audio" : audio);
            QVERIFY2(!out.isEmpty(), qPrintable(what + ": " + w.log_->toPlainText().right(300)));
            const QJsonArray s = streams(out);
            QJsonObject video, sound;
            int others = 0;
            for (const QJsonValue &v : s) {
                const QString type = v.toObject().value("codec_type").toString();
                if (type == "video")
                    video = v.toObject();
                else if (type == "audio")
                    sound = v.toObject();
                else
                    ++others;
            }
            QCOMPARE(video.value("codec_name").toString(), streamCodec.value(family));
            // the encoder written in the file must be the one the window announced
            const QString tag = video.value("tags").toObject().value("encoder").toString();
            QVERIFY2(tag.isEmpty() || tag.contains(w.codec_->encoder), qPrintable(what + ": " + tag));
            QCOMPARE(sound.isEmpty(), audio.isEmpty());
            QCOMPARE(others, 0);   // subtitles are not copied
            ++encodes;
        }
    }
    qInfo() << encodes << "encodes checked";
}

void TestVideoCompressor::scalesPortraitPhoneVideos()
{
    const QString out = compress(path("rotated.mp4"), "mp4", "h264", "off", {}, "aac", 10, 540);
    QVERIFY(!out.isEmpty());
    const QJsonObject video = streams(out).first().toObject();
    QCOMPARE(video.value("width").toInt(), 540);
    QCOMPARE(video.value("height").toInt(), 960);
}

void TestVideoCompressor::usesVideoNotCoverArt()
{
    const QString out = compress(path("cover.mp4"), "mp4", "h264", "off", {}, "aac");
    QVERIFY(!out.isEmpty());
    const QJsonArray s = streams(out);
    QCOMPARE(s.size(), 2);
    QCOMPARE(s[0].toObject().value("width").toInt(), 1280);
}

void TestVideoCompressor::placesKeyframesWithClosedGop()
{
    MainWindow &w = *win_;
    for (const VideoCodec &codec : videoCodecs()) {
        if (!w.encoders_->available().contains(codec.encoder))
            continue;
        const QString mode = codec.hardware() ? "manual" : "off";
        if (codec.family == "vvc")
            select(w.speedCombo_, "fast");   // VVenC is very slow
        for (int keyint : {10, 2}) {
            const QString out = compress(path("20s.mp4"), "mkv", codec.family, mode, codec.vendor, {}, keyint);
            QVERIFY2(!out.isEmpty(), qPrintable(codec.encoder));
            const QList<double> kf = keyframeTimes(out);
            double maxGap = 0;
            for (int i = 1; i < kf.size(); ++i)
                maxGap = std::max(maxGap, kf[i] - kf[i - 1]);
            // VVenC aligns keyframes to its 32-frame structure: up to ~7% later
            QVERIFY2(maxGap <= keyint * 1.08 + 0.05 && kf.size() >= int(20 / (keyint * 1.08)),
                     qPrintable(QString("%1 every %2 s: %3 keyframes, max gap %4 s")
                                    .arg(codec.encoder).arg(keyint).arg(kf.size()).arg(maxGap)));
            if (keyint == 10 && (codec.family == "h264" || codec.family == "hevc" || codec.family == "vvc")) {
                // closed GOP: every keyframe is an IDR (H.264: 5; HEVC: 19-20, CRA 21; VVC: 7-8, CRA 9)
                const QList<int> types = nalTypes(out);
                const auto count = [&](std::initializer_list<int> values) {
                    return int(std::count_if(types.begin(), types.end(),
                                             [&](int t) { return std::find(values.begin(), values.end(), t) != values.end(); }));
                };
                const int idr = codec.family == "h264" ? count({5}) : codec.family == "hevc" ? count({19, 20}) : count({7, 8});
                const int cra = codec.family == "hevc" ? count({21}) : codec.family == "vvc" ? count({9}) : 0;
                QVERIFY2(cra == 0 && idr >= kf.size(),
                         qPrintable(QString("%1: IDR %2, CRA %3, keyframes %4").arg(codec.encoder).arg(idr).arg(cra).arg(kf.size())));
            }
        }
        select(w.speedCombo_, "balanced");
    }
}

namespace {

// EBML variable-length integer (Matroska)
quint64 readVint(QFile &f, bool keepMarker)
{
    char first = 0;
    f.getChar(&first);
    const quint8 b = quint8(first);
    int length = 1;
    while (length <= 8 && !(b & (0x80 >> (length - 1))))
        ++length;
    quint64 value = keepMarker ? b : (b & (0xFF >> length));
    for (int i = 1; i < length; ++i) {
        char c = 0;
        f.getChar(&c);
        value = (value << 8) | quint8(c);
    }
    return value;
}

// Order of the top-level elements inside the Matroska Segment
QStringList mkvElements(const QString &path)
{
    static const QMap<quint64, QString> names = {{0x114D9B74, "SeekHead"}, {0x1549A966, "Info"},
                                                 {0x1654AE6B, "Tracks"}, {0x1C53BB6B, "Cues"},
                                                 {0x1F43B675, "Cluster"}, {0x1254C367, "Tags"}};
    QStringList order;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return order;
    readVint(f, true);
    const qint64 headerSize = qint64(readVint(f, false));   // read before f.pos(): the size field moves it
    f.seek(f.pos() + headerSize);                           // skip the EBML header
    readVint(f, true);
    readVint(f, false);                              // Segment
    while (!f.atEnd()) {
        const quint64 id = readVint(f, true);
        const quint64 size = readVint(f, false);
        const QString name = names.value(id, "other");
        if (name != "other" && (order.isEmpty() || order.last() != name))
            order << name;
        f.seek(f.pos() + qint64(size));
    }
    return order;
}

QStringList mp4Boxes(const QString &path)
{
    QStringList order;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return order;
    while (f.bytesAvailable() >= 8) {
        const QByteArray head = f.read(8);
        quint64 size = qFromBigEndian<quint32>(head.constData());
        qint64 headerSize = 8;
        if (size == 1) {
            size = qFromBigEndian<quint64>(f.read(8).constData());
            headerSize = 16;
        }
        order << QString::fromLatin1(head.mid(4, 4));
        f.seek(f.pos() + qint64(size) - headerSize);
    }
    return order;
}

} // namespace

void TestVideoCompressor::writesSeekIndexAtStart()
{
    QString out = compress(path("20s.mp4"), "mp4", "h264", "off", {}, "aac");
    QVERIFY(!out.isEmpty());
    const QStringList boxes = mp4Boxes(out);
    QVERIFY2(boxes.indexOf("moov") >= 0 && boxes.indexOf("moov") < boxes.indexOf("mdat"), qPrintable(boxes.join(' ')));

    for (const auto &[format, family] : {std::pair{"mkv", "h264"}, std::pair{"webm", "vp9"}}) {
        if (!usable(family) || !win_->encoders_->available().contains("libopus"))
            continue;
        out = compress(path("20s.mp4"), format, family, "off", {}, "opus");
        QVERIFY(!out.isEmpty());
        const QStringList elements = mkvElements(out);
        QVERIFY2(elements.indexOf("Cues") >= 0 && elements.indexOf("Cues") < elements.indexOf("Cluster"),
                 qPrintable(QString(format) + ": " + elements.join(' ')));
    }
}

void TestVideoCompressor::estimatesAccurately_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("family");
    QTest::addColumn<QString>("mode");
    QTest::addColumn<int>("keyint");
    for (const char *input : {"static.mp4", "long.mp4"})
        for (int keyint : {10, 2}) {
            for (const char *family : {"h264", "av1"})
                QTest::addRow("%s %s CPU %d s", input, family, keyint) << QString(input) << QString(family) << QString("off") << keyint;
            QTest::addRow("%s h264 GPU %d s", input, keyint) << QString(input) << QString("h264") << QString("auto") << keyint;
        }
}

void TestVideoCompressor::estimatesAccurately()
{
    QFETCH(QString, input);
    QFETCH(QString, family);
    QFETCH(QString, mode);
    QFETCH(int, keyint);
    MainWindow &w = *win_;
    if (mode == "auto" && hardwareVendors(family).isEmpty())
        QSKIP("no GPU encoder on this PC");
    if (!usable(family))
        QSKIP("no encoder for this codec in this FFmpeg");

    w.setInput(path(input));
    select(w.formatCombo_, "mkv");
    select(w.vcodecCombo_, family);
    select(w.accelCombo_, mode);
    select(w.acodecCombo_, QString());
    select(w.keyintCombo_, keyint);
    QVERIFY(QTest::qWaitFor([this] { return settled(); }, 300000));
    QVERIFY2(w.estBps_, qPrintable(w.sizeDetail_->text()));
    const double estimate = w.estimatedBytes();
    const QString out = path("estimate.mkv");
    QFile::remove(out);
    w.outEdit_->setText(out);
    w.start();
    QVERIFY(QTest::qWaitFor([&] { return !w.encoding_; }, 600000));
    const double real = double(QFileInfo(out).size());
    const double error = estimate / real - 1;
    qInfo().noquote() << QString("%1 %2 %3: estimate %4 KB, real %5 KB, error %6%")
                             .arg(input, w.codec_->encoder).arg(keyint).arg(estimate / 1024, 0, 'f', 0)
                             .arg(real / 1024, 0, 'f', 0).arg(error * 100, 0, 'f', 1);
    QVERIFY(std::abs(error) < 0.12);   // the README promises roughly ±10%
}

void TestVideoCompressor::fitsToLimit()
{
    MainWindow &w = *win_;
    if (!win_->encoders_->available().contains("libx265"))
        QSKIP("no x265 in this FFmpeg");
    messages_.clear();
    w.setInput(path("long.mp4"));
    select(w.formatCombo_, "mp4");
    select(w.vcodecCombo_, "hevc");
    select(w.accelCombo_, "off");
    select(w.acodecCombo_, "aac");
    select(w.keyintCombo_, 10);
    QVERIFY(QTest::qWaitFor([this] { return settled(); }, 300000));
    w.limitSpin_->setValue(2.0);
    w.fitToLimit();
    QVERIFY(QTest::qWaitFor([&] { return !w.fit_ && settled(); }, 300000));
    QVERIFY2(messages_.isEmpty(), qPrintable(messages_.join(", ")));
    QVERIFY(w.estimatedBytes() <= 2.0 * MB);

    const QString out = path("fit.mp4");
    QFile::remove(out);
    w.outEdit_->setText(out);
    w.start();
    QVERIFY(QTest::qWaitFor([&] { return !w.encoding_; }, 600000));
    const qint64 size = QFileInfo(out).size();
    qInfo().noquote() << "Fit to 2 MB:" << w.qualityLabel_->text() << "->" << fmtMb(double(size));
    QVERIFY(size > 0 && size <= 2 * MB);
}

// No network: the version logic and GitHub's JSON are checked directly, the notice by emitting the signal.
void TestVideoCompressor::notifiesAboutUpdates()
{
    QVERIFY(UpdateChecker::isNewer("v9.9.10", "9.9.9"));    // numeric, not alphabetical
    QVERIFY(UpdateChecker::isNewer("v10.0.0", "9.9.9"));
    QVERIFY(!UpdateChecker::isNewer("v9.9.9", "9.9.9"));
    QVERIFY(!UpdateChecker::isNewer("v9.9", "9.9.0"));
    QVERIFY(!UpdateChecker::isNewer("v9.8.0", "9.9.9"));
    QVERIFY(!UpdateChecker::isNewer("nightly", "9.9.9"));

    const QByteArray json = R"({
        "tag_name": "v9.9.10",
        "html_url": "https://github.com/Andrea332/VideoCompressor/releases/tag/v9.9.10",
        "assets": [
            {"name": "VideoCompressor-9.9.10-win64.zip", "browser_download_url": "https://example.com/a.zip"},
            {"name": "VideoCompressor-9.9.10-win64.exe", "browser_download_url": "https://example.com/setup.exe",
             "digest": "sha256:ABCDEF0123"},
            {"name": "VideoCompressor-9.9.10-macos-arm64.dmg", "browser_download_url": "https://example.com/a.dmg"}
        ]})";
    const auto info = UpdateChecker::parseLatestRelease(json, "9.9.9");
    QVERIFY(info);
    QCOMPARE(info->version, QString("9.9.10"));
    QCOMPARE(info->installerName, QString("VideoCompressor-9.9.10-win64.exe"));
    QCOMPARE(info->installerUrl, QUrl("https://example.com/setup.exe"));
    QCOMPARE(info->installerSha256, QString("abcdef0123"));
    QVERIFY(!UpdateChecker::parseLatestRelease(json, "9.9.10"));

    MainWindow &w = *win_;
    QVERIFY(!w.autoUpdateCheck_->isChecked());   // turned off in main(): tests never go online
    QVERIFY(!w.updateBar_->isVisible());
    emit w.updater_->updateAvailable(*info);
    QVERIFY(w.updateBar_->isVisible());
    QVERIFY2(w.updateText_->text().contains("9.9.10") && w.updateText_->text().contains("9.9.9"),
             qPrintable(w.updateText_->text()));
    // not installed with the installer (no uninstaller next to the test executable): download page
    QCOMPARE(w.updateBtn_->text(), QString("Download"));
    QTest::mouseClick(w.laterBtn_, Qt::LeftButton);
    QVERIFY(!w.updateBar_->isVisible());
}

// A window destroyed while its children still emit signals must not receive them any more: FFmpeg
// processes finishing (preview, estimate, GPU tests) and the path field, which emits editingFinished
// when it loses the focus as the window closes. This used to corrupt the heap at exit (seen on Linux;
// the sanitizer job reports it every time).
void TestVideoCompressor::survivesClosingWhileBusy()
{
    for (int delayMs : {0, 300, 800}) {
        auto window = std::make_unique<MainWindow>();
        window->showMessage = [](QMessageBox::Icon, const QString &, const QString &) {};
        window->show();
        window->activateWindow();
        window->setInput(path("long.mp4"));
        window->inEdit_->setFocus();
        QTest::qWait(delayMs);
        QVERIFY(window->previewProc_ || window->estTimer_->isActive() || window->estimator_->running()
                || delayMs > 0);
    }
}

// Saves a screenshot to VC_SCREENSHOT_DIR, if set (use QT_QPA_FONTDIR=C:/Windows/Fonts to get text offscreen).
// With VC_SCREENSHOT_VIDEO it is the README picture: that video, default settings, a fresh-looking window
// (see the workflow, which runs it on Windows with the real platform plugin).
void TestVideoCompressor::rendersWindow()
{
    MainWindow &w = *win_;
    const QString video = qEnvironmentVariable("VC_SCREENSHOT_VIDEO");
    w.setInput(video.isEmpty() ? path("long.mp4") : video);
    select(w.formatCombo_, "mp4");
    select(w.vcodecCombo_, video.isEmpty() ? "hevc" : "h264");
    // the CI machine has no GPU: "Off" reads better there than "no GPU in this PC can encode H.264"
    select(w.accelCombo_, video.isEmpty() ? "auto" : "off");
    if (!video.isEmpty()) {
        select(w.acodecCombo_, "aac");
        select(w.keyintCombo_, 10);
        w.limitSpin_->setValue(25);
        w.autoUpdateCheck_->setChecked(true);   // as a new user sees it (this only changes the tests' settings)
        w.log_->clear();
        w.progress_->setValue(0);
        w.openBtn_->setEnabled(false);
    }
    QVERIFY(QTest::qWaitFor([this] { return settled(); }, 300000));
    QVERIFY(QTest::qWaitFor([&] { return w.preview_->isVisible(); }, 30000));
    const QPixmap shot = w.grab();
    QVERIFY(!shot.isNull());
    if (const QString dir = qEnvironmentVariable("VC_SCREENSHOT_DIR"); !dir.isEmpty())
        QVERIFY(shot.save(dir + "/window.png"));
}

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QApplication::setOrganizationName("VideoCompressorTests");
    QApplication::setApplicationName("tst_video_compressor");
    QApplication::setApplicationVersion("9.9.9");
    // preferences in a temporary file, not the user's, with the automatic update check off
    QTemporaryDir settingsDir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings().setValue("updates/autoCheck", false);
    TestVideoCompressor test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_video_compressor.moc"
