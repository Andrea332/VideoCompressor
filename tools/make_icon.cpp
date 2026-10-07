// Renders the app icon from SVG into a multi-size .ico (PNG-compressed images, as Windows Vista and later accept).
// Small sizes use a simplified drawing, because the details of the full one turn into noise at 16-24 px.
//
// Usage: make_icon <icon.svg> <icon-small.svg> <out.ico>

#include <QBuffer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QtEndian>

#include <cstdio>

namespace {

void put16(QByteArray &out, quint16 value)
{
    char bytes[2];
    qToLittleEndian(value, bytes);
    out.append(bytes, 2);
}

void put32(QByteArray &out, quint32 value)
{
    char bytes[4];
    qToLittleEndian(value, bytes);
    out.append(bytes, 4);
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    if (argc != 4) {
        std::fprintf(stderr, "usage: make_icon <icon.svg> <icon-small.svg> <out.ico>\n");
        return 2;
    }
    QSvgRenderer full(QString::fromLocal8Bit(argv[1]));
    QSvgRenderer small(QString::fromLocal8Bit(argv[2]));
    if (!full.isValid() || !small.isValid()) {
        std::fprintf(stderr, "cannot read the SVG files\n");
        return 1;
    }

    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};
    QList<QByteArray> images;
    for (int size : sizes) {
        QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        (size <= 24 ? small : full).render(&painter);
        painter.end();
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        images << png;
    }

    // ICONDIR header, one 16-byte ICONDIRENTRY per image, then the images
    QByteArray ico;
    put16(ico, 0);
    put16(ico, 1);
    put16(ico, quint16(images.size()));
    quint32 offset = 6 + 16 * quint32(images.size());
    for (int i = 0; i < images.size(); ++i) {
        const int size = sizes[i];
        ico.append(char(size >= 256 ? 0 : size));   // 0 means 256
        ico.append(char(size >= 256 ? 0 : size));
        ico.append(char(0));                         // no palette
        ico.append(char(0));
        put16(ico, 1);                               // color planes
        put16(ico, 32);                              // bits per pixel
        put32(ico, quint32(images[i].size()));
        put32(ico, offset);
        offset += quint32(images[i].size());
    }
    for (const QByteArray &image : images)
        ico += image;

    QFile out(QString::fromLocal8Bit(argv[3]));
    if (!out.open(QIODevice::WriteOnly) || out.write(ico) != ico.size()) {
        std::fprintf(stderr, "cannot write %s\n", argv[3]);
        return 1;
    }
    std::printf("%s: %lld images, %lld bytes\n", argv[3], qint64(images.size()), qint64(ico.size()));
    return 0;
}
