#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

#include <optional>

// The processor this build is for, as in the name of its Windows installer (set by CMakeLists.txt)
#ifndef INSTALLER_PLATFORM
#define INSTALLER_PLATFORM "win64"
#endif

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

struct UpdateInfo {
    QString version;            // e.g. "2.3.0"
    QUrl page;                  // release page, with the notes and every download
    QUrl installerUrl;          // Windows installer, empty if the release has none
    QString installerName;
    QString installerSha256;    // from GitHub's asset digest, empty if not provided
};

// Asks GitHub for the latest release of the app and, for the Windows installer, downloads it.
class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    static constexpr const char *LATEST_RELEASE_API =
        "https://api.github.com/repos/Andrea332/VideoCompressor/releases/latest";
    // the installer of this build's Windows version among the files of a release (x64 or ARM64)
    static constexpr const char *INSTALLER_SUFFIX = "-" INSTALLER_PLATFORM ".exe";

    explicit UpdateChecker(QObject *parent = nullptr);

    void check();
    void download(const UpdateInfo &info);
    void cancelDownload();
    bool downloading() const { return download_ != nullptr; }

    // true if candidate (e.g. "v2.3.0") is a later version than current (e.g. "2.2.0")
    static bool isNewer(const QString &candidate, const QString &current);
    // the release described by GitHub's JSON, if it is newer than current
    static std::optional<UpdateInfo> parseLatestRelease(const QByteArray &json, const QString &current);

signals:
    void updateAvailable(const UpdateInfo &info);
    void upToDate();
    void failed(const QString &message);
    void downloadProgress(qint64 received, qint64 total);
    void downloaded(const QString &path);

private:
    QNetworkAccessManager *net_;
    QNetworkReply *download_ = nullptr;
    QFile *file_ = nullptr;
};
