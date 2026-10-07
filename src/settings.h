#pragma once

#include <QSettings>
#include <QString>

#include <memory>

// The portable versions keep their preferences in this file next to the program (on Linux at the top of the
// extracted folder, on macOS next to the app): it comes with them, and finding it is what turns this on.
inline constexpr const char *PORTABLE_SETTINGS_FILE = "VideoCompressor.ini";

// Where the portable settings file is for a program in appDir (QCoreApplication::applicationDirPath())
QString portableSettingsPath(const QString &appDir);

// The app's preferences: the portable file if there is one, otherwise the system's usual place
// (the registry on Windows, ~/Library/Preferences on macOS, ~/.config on Linux)
std::unique_ptr<QSettings> appSettings();
