// Renders the app icon from SVG for every platform, into <output-dir>:
//   icon.ico   Windows, 16-256 px (PNG-compressed images, as Windows Vista and later accept)
//   icon.icns  macOS, 16-1024 px
//   icon.png   Linux, 256 px
// Small sizes use a simplified drawing, because the details of the full one turn into noise at 16-24 px.
//
// Usage: make_icon <icon.svg> <icon-small.svg> <output-dir>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QtEndian>

#include <cstdio>

namespace {

QSvgRenderer *fullIcon;
QSvgRenderer *smallIcon;

QByteArray renderPng(int size)
{
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    (size <= 24 ? smallIcon : fullIcon)->render(&painter);
    painter.end();
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return png;
}

template<typename T>
void append(QByteArray &out, T value, bool bigEndian)
{
    char bytes[sizeof(T)];
    if (bigEndian)
        qToBigEndian(value, bytes);
    else
        qToLittleEndian(value, bytes);
    out.append(bytes, sizeof(T));
}

QByteArray makeIco()
{
    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};
    QList<QByteArray> images;
    for (int size : sizes)
        images << renderPng(size);
    // ICONDIR header, one 16-byte ICONDIRENTRY per image, then the images
    QByteArray ico;
    append<quint16>(ico, 0, false);
    append<quint16>(ico, 1, false);
    append<quint16>(ico, quint16(images.size()), false);
    quint32 offset = 6 + 16 * quint32(images.size());
    for (int i = 0; i < images.size(); ++i) {
        ico.append(char(sizes[i] >= 256 ? 0 : sizes[i]));   // 0 means 256
        ico.append(char(sizes[i] >= 256 ? 0 : sizes[i]));
        ico.append(char(0));                                  // no palette
        ico.append(char(0));
        append<quint16>(ico, 1, false);                       // color planes
        append<quint16>(ico, 32, false);                      // bits per pixel
        append<quint32>(ico, quint32(images[i].size()), false);
        append<quint32>(ico, offset, false);
        offset += quint32(images[i].size());
    }
    for (const QByteArray &image : images)
        ico += image;
    return ico;
}

QByteArray makeIcns()
{
    // PNG-based icon types: 16, 32, 64, 128, 256, 512, 1024 px and their @2x variants
    const QList<QPair<const char *, int>> types = {
        {"icp4", 16},  {"icp5", 32},  {"icp6", 64},  {"ic07", 128}, {"ic08", 256}, {"ic09", 512},
        {"ic10", 1024}, {"ic11", 32}, {"ic12", 64}, {"ic13", 256}, {"ic14", 512}};
    QByteArray body;
    for (const auto &[type, size] : types) {
        const QByteArray png = renderPng(size);
        body.append(type, 4);
        append<quint32>(body, quint32(8 + png.size()), true);
        body += png;
    }
    QByteArray icns("icns");
    append<quint32>(icns, quint32(8 + body.size()), true);
    return icns + body;
}

bool write(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
        std::fprintf(stderr, "cannot write %s\n", qPrintable(path));
        return false;
    }
    std::printf("%s: %lld bytes\n", qPrintable(path), qint64(data.size()));
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    if (argc != 4) {
        std::fprintf(stderr, "usage: make_icon <icon.svg> <icon-small.svg> <output-dir>\n");
        return 2;
    }
    QSvgRenderer full(QString::fromLocal8Bit(argv[1]));
    QSvgRenderer small(QString::fromLocal8Bit(argv[2]));
    if (!full.isValid() || !small.isValid()) {
        std::fprintf(stderr, "cannot read the SVG files\n");
        return 1;
    }
    fullIcon = &full;
    smallIcon = &small;
    const QDir out(QString::fromLocal8Bit(argv[3]));
    const bool ok = write(out.filePath("icon.ico"), makeIco()) && write(out.filePath("icon.icns"), makeIcns())
                    && write(out.filePath("icon.png"), renderPng(256));
    return ok ? 0 : 1;
}
