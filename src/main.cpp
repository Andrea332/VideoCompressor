// Video Compressor - Qt app to compress videos at a chosen quality, with a preview of the final file size.
//
// The estimate is made by actually encoding a few short samples of the video with the chosen settings,
// so it is reliable even with very different kinds of content (screen recordings, camera footage,
// animations...). Encoding is done by FFmpeg, bundled with the app.

#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("Andrea Galet");   // where QSettings keeps the preferences (see settings.h)
    QApplication::setApplicationName("Video Compressor");
    QApplication::setApplicationVersion(APP_VERSION);

    MainWindow window;
    window.show();
    const QStringList args = QApplication::arguments();
    if (args.size() > 1)
        window.setInput(args.at(1));
    return QApplication::exec();
}
