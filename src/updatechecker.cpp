#include "updatechecker.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QVersionNumber>

namespace {

QVersionNumber parseVersion(QString text)
{
    text = text.trimmed();
    if (text.startsWith('v', Qt::CaseInsensitive))
        text = text.mid(1);
    return QVersionNumber::fromString(text);
}

QNetworkRequest request(const QUrl &url)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "VideoCompressor/" + QCoreApplication::applicationVersion());
    req.setTransferTimeout(30000);
    return req;
}

} // namespace

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent), net_(new QNetworkAccessManager(this))
{
}

bool UpdateChecker::isNewer(const QString &candidate, const QString &current)
{
    const QVersionNumber latest = parseVersion(candidate), mine = parseVersion(current);
    return !latest.isNull() && !mine.isNull() && QVersionNumber::compare(latest.normalized(), mine.normalized()) > 0;
}

std::optional<UpdateInfo> UpdateChecker::parseLatestRelease(const QByteArray &json, const QString &current)
{
    const QJsonObject release = QJsonDocument::fromJson(json).object();
    const QString tag = release.value("tag_name").toString();
    if (!isNewer(tag, current))
        return std::nullopt;
    UpdateInfo info;
    info.version = parseVersion(tag).toString();
    info.page = QUrl(release.value("html_url").toString());
    for (const QJsonValue &value : release.value("assets").toArray()) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value("name").toString();
        if (name.endsWith(INSTALLER_SUFFIX, Qt::CaseInsensitive)) {
            info.installerName = name;
            info.installerUrl = QUrl(asset.value("browser_download_url").toString());
            const QString digest = asset.value("digest").toString();   // "sha256:<hex>"
            if (digest.startsWith("sha256:"))
                info.installerSha256 = digest.mid(7).toLower();
        }
    }
    return info;
}

void UpdateChecker::check()
{
    QNetworkRequest req = request(QUrl(LATEST_RELEASE_API));
    req.setRawHeader("Accept", "application/vnd.github+json");
    QNetworkReply *reply = net_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit failed(reply->errorString());
            return;
        }
        if (const auto info = parseLatestRelease(reply->readAll(), QCoreApplication::applicationVersion()))
            emit updateAvailable(*info);
        else
            emit upToDate();
    });
}

void UpdateChecker::download(const UpdateInfo &info)
{
    cancelDownload();
    const QString dir = QDir::temp().filePath("VideoCompressor-update");
    QDir().mkpath(dir);
    file_ = new QFile(QDir(dir).filePath(info.installerName), this);
    if (!file_->open(QIODevice::WriteOnly)) {
        emit failed(file_->errorString());
        delete file_;
        file_ = nullptr;
        return;
    }
    download_ = net_->get(request(info.installerUrl));   // GitHub redirects to its file storage
    connect(download_, &QNetworkReply::readyRead, this, [this] { file_->write(download_->readAll()); });
    connect(download_, &QNetworkReply::downloadProgress, this, &UpdateChecker::downloadProgress);
    connect(download_, &QNetworkReply::finished, this, [this, sha256 = info.installerSha256] {
        QNetworkReply *reply = download_;
        QFile *file = file_;
        download_ = nullptr;
        file_ = nullptr;
        reply->deleteLater();
        file->write(reply->readAll());
        file->close();
        const QString path = file->fileName();
        delete file;
        if (reply->error() != QNetworkReply::NoError) {
            QFile::remove(path);
            emit failed(reply->errorString());
            return;
        }
        if (!sha256.isEmpty()) {
            QFile check(path);
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (!check.open(QIODevice::ReadOnly) || !hash.addData(&check) || hash.result().toHex() != sha256.toLatin1()) {
                check.close();
                QFile::remove(path);
                emit failed("the downloaded installer is damaged (checksum mismatch)");
                return;
            }
        }
        emit downloaded(path);
    });
}

void UpdateChecker::cancelDownload()
{
    if (!download_)
        return;
    QNetworkReply *reply = download_;
    download_ = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
    const QString path = file_->fileName();
    file_->close();
    delete file_;
    file_ = nullptr;
    QFile::remove(path);
}
