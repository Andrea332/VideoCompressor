#include "settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

QString portableSettingsPath(const QString &appDir)
{
#if defined(Q_OS_WIN)
    const QString dir = appDir;
#elif defined(Q_OS_MACOS)
    const QString dir = appDir + "/../../..";   // VideoCompressor.app/Contents/MacOS: next to the app
#else
    const QString dir = appDir + "/../..";      // usr/bin: the top of the extracted folder
#endif
    return QDir::cleanPath(dir + "/" + PORTABLE_SETTINGS_FILE);
}

std::unique_ptr<QSettings> appSettings()
{
    const QString portable = portableSettingsPath(QCoreApplication::applicationDirPath());
    if (QFileInfo::exists(portable))
        return std::make_unique<QSettings>(portable, QSettings::IniFormat);
    return std::make_unique<QSettings>();
}
