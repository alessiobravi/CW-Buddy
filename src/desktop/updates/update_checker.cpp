#include "update_checker.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>

#include <array>

namespace cwassistant::desktop {
namespace {

constexpr auto kManifestUrl =
    "https://github.com/alessiobravi/CW-Buddy/releases/download/"
    "continuous/latest.json";

[[nodiscard]] std::array<int, 3> parseVersion(const QString& text) noexcept {
  std::array<int, 3> version{0, 0, 0};
  static const QRegularExpression pattern(
      QStringLiteral("^(\\d+)\\.(\\d+)\\.(\\d+)$"));
  const auto match = pattern.match(text);
  if (!match.hasMatch()) return version;
  version[0] = match.captured(1).toInt();
  version[1] = match.captured(2).toInt();
  version[2] = match.captured(3).toInt();
  return version;
}

[[nodiscard]] bool isNewerVersion(const QString& candidate,
                                  const QString& current) noexcept {
  return parseVersion(candidate) > parseVersion(current);
}

}  // namespace

namespace update_detail {

bool isIncompleteTransfer(const qint64 declared_content_length,
                          const qint64 received_bytes) noexcept {
  if (declared_content_length <= 0) return false;
  return received_bytes < declared_content_length;
}

bool isTransientFailure(const QNetworkReply::NetworkError error,
                        const int http_status) noexcept {
  if (http_status == 404 || http_status == 408 || http_status == 425 ||
      http_status == 429 || (http_status >= 500 && http_status <= 599)) {
    return true;
  }
  switch (error) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::UnknownProxyError:
    case QNetworkReply::UnknownServerError:
      return true;
    default:
      return false;
  }
}

int retryDelayMs(const int completed_attempts) noexcept {
  if (completed_attempts <= 1) return 400;
  return completed_attempts == 2 ? 1'200 : 2'500;
}

UpdateActionVisibility updateActionVisibility(
    const bool update_available, const bool platform_supported,
    const bool download_verified) noexcept {
  if (download_verified) {
    return {.row = true, .download = false, .verified_download = true};
  }
  const bool can_download = update_available && platform_supported;
  return {.row = can_download,
          .download = can_download,
          .verified_download = false};
}

}  // namespace update_detail

UpdateChecker::UpdateChecker(QObject* parent)
    : QObject(parent),
      current_version_(QCoreApplication::applicationVersion()) {
  QSettings settings;
  auto_check_enabled_ =
      settings.value(QStringLiteral("updates/autoCheckEnabled"), true)
          .toBool();
  const auto last_checked_text =
      settings.value(QStringLiteral("updates/lastCheckedIso")).toString();
  if (!last_checked_text.isEmpty()) {
    last_checked_ = QDateTime::fromString(last_checked_text, Qt::ISODate);
  }
}

bool UpdateChecker::autoCheckEnabled() const noexcept {
  return auto_check_enabled_;
}
void UpdateChecker::setAutoCheckEnabled(const bool value) {
  if (auto_check_enabled_ == value) return;
  auto_check_enabled_ = value;
  QSettings settings;
  settings.setValue(QStringLiteral("updates/autoCheckEnabled"), value);
  emit autoCheckEnabledChanged();
}
bool UpdateChecker::checking() const noexcept { return checking_; }
bool UpdateChecker::updateAvailable() const noexcept {
  return update_available_;
}
const QString& UpdateChecker::currentVersion() const noexcept {
  return current_version_;
}
const QString& UpdateChecker::latestVersion() const noexcept {
  return latest_version_;
}
QString UpdateChecker::lastCheckedText() const {
  return last_checked_.isValid()
             ? last_checked_.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
             : QStringLiteral("Never checked");
}
const QString& UpdateChecker::statusMessage() const noexcept {
  return status_message_;
}
bool UpdateChecker::downloading() const noexcept { return downloading_; }
double UpdateChecker::downloadProgress() const noexcept {
  return download_progress_;
}
bool UpdateChecker::downloadVerified() const noexcept {
  return download_verified_;
}
const QString& UpdateChecker::downloadedFilePath() const noexcept {
  return downloaded_file_path_;
}
bool UpdateChecker::platformSupported() const noexcept {
  return !platformArtifactKey().isEmpty();
}
bool UpdateChecker::updateActionVisible() const noexcept {
  return update_detail::updateActionVisibility(
             update_available_, platformSupported(), download_verified_)
      .row;
}
bool UpdateChecker::downloadActionVisible() const noexcept {
  return update_detail::updateActionVisibility(
             update_available_, platformSupported(), download_verified_)
      .download;
}
bool UpdateChecker::verifiedDownloadActionsVisible() const noexcept {
  return update_detail::updateActionVisibility(
             update_available_, platformSupported(), download_verified_)
      .verified_download;
}

void UpdateChecker::setStatus(QString message) {
  status_message_ = std::move(message);
}

QString UpdateChecker::platformArtifactKey() {
#if defined(Q_OS_WIN)
  return QStringLiteral("windows11-x64-installer");
#elif defined(Q_OS_MACOS)
  return QSysInfo::currentCpuArchitecture().contains(QStringLiteral("arm"))
             ? QStringLiteral("macos-arm64")
             : QStringLiteral("macos-x64");
#elif defined(Q_OS_LINUX)
  return QFile::exists(QStringLiteral("/etc/debian_version"))
             ? QStringLiteral("debian-ubuntu-x64")
             : QStringLiteral("linux-x64-portable");
#else
  return QString();
#endif
}

void UpdateChecker::checkForUpdates() {
  if (checking_) return;
  checking_ = true;
  setStatus(QStringLiteral("Checking for updates…"));
  emit stateChanged();
  manifest_attempts_ = 0;
  requestManifest();
}

void UpdateChecker::requestManifest() {
  ++manifest_attempts_;
  auto* reply = network_.get(QNetworkRequest(QUrl(QString::fromLatin1(
      kManifestUrl))));
  connect(reply, &QNetworkReply::finished, this,
          [this, reply] { handleManifestReply(reply); });
}

void UpdateChecker::handleManifestReply(QNetworkReply* reply) {
  if (reply->error() != QNetworkReply::NoError) {
    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool retry =
        manifest_attempts_ < update_detail::kMaximumAttempts &&
        update_detail::isTransientFailure(reply->error(), status);
    const QString error = reply->errorString();
    reply->deleteLater();
    if (retry) {
      setStatus(QStringLiteral(
          "Release is being published; retrying update check…"));
      emit stateChanged();
      QTimer::singleShot(update_detail::retryDelayMs(manifest_attempts_), this,
                         [this] { requestManifest(); });
      return;
    }
    checking_ = false;
    setStatus(QStringLiteral("Update check failed: ") + error);
    emit stateChanged();
    return;
  }
  checking_ = false;
  const auto document = QJsonDocument::fromJson(reply->readAll());
  reply->deleteLater();
  if (!document.isObject() ||
      !document.object().contains(QStringLiteral("version"))) {
    setStatus(QStringLiteral(
        "Update check failed: the published manifest was malformed"));
    emit stateChanged();
    return;
  }
  last_manifest_ = document.object();
  latest_version_ = last_manifest_.value(QStringLiteral("version")).toString();
  update_available_ = isNewerVersion(latest_version_, current_version_);
  last_checked_ = QDateTime::currentDateTime();
  QSettings settings;
  settings.setValue(QStringLiteral("updates/lastCheckedIso"),
                    last_checked_.toString(Qt::ISODate));
  setStatus(update_available_
                ? QStringLiteral("Update available: %1 (you have %2)")
                      .arg(latest_version_, current_version_)
                : QStringLiteral("Up to date (%1)").arg(current_version_));
  emit stateChanged();
}

void UpdateChecker::downloadUpdate() {
  if (downloading_) return;
  if (last_manifest_.isEmpty()) {
    setStatus(QStringLiteral("Check for updates first"));
    emit stateChanged();
    return;
  }
  const auto key = platformArtifactKey();
  if (key.isEmpty()) {
    setStatus(QStringLiteral(
        "Guided downloads are not available for this platform yet"));
    emit stateChanged();
    return;
  }
  const auto artifact_url =
      last_manifest_.value(QStringLiteral("artifacts")).toObject()
          .value(key)
          .toString();
  const auto checksums_url =
      last_manifest_.value(QStringLiteral("checksums")).toString();
  if (artifact_url.isEmpty() || checksums_url.isEmpty()) {
    setStatus(QStringLiteral(
        "The published manifest has no download for this platform"));
    emit stateChanged();
    return;
  }
  downloading_ = true;
  download_progress_ = 0.0;
  download_verified_ = false;
  downloaded_file_path_.clear();
  pending_artifact_url_ = artifact_url;
  pending_checksums_url_ = checksums_url;
  checksums_attempts_ = 0;
  artifact_attempts_ = 0;
  setStatus(QStringLiteral("Downloading checksums…"));
  emit stateChanged();

  // Fetched sequentially, not concurrently: the artifact reply must not be
  // able to finish before pending_checksums_text_ is populated, since
  // handleArtifactReply() needs it to verify the download.
  requestChecksums();
}

void UpdateChecker::requestChecksums() {
  ++checksums_attempts_;
  auto* checksums_reply = network_.get(
      QNetworkRequest(QUrl(pending_checksums_url_)));
  connect(checksums_reply, &QNetworkReply::finished, this,
          [this, checksums_reply] { handleChecksumsReply(checksums_reply); });
}

void UpdateChecker::handleChecksumsReply(QNetworkReply* reply) {
  if (reply->error() != QNetworkReply::NoError) {
    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool retry =
        checksums_attempts_ < update_detail::kMaximumAttempts &&
        update_detail::isTransientFailure(reply->error(), status);
    const QString error = reply->errorString();
    reply->deleteLater();
    if (retry) {
      setStatus(QStringLiteral(
          "Release files are being published; retrying checksums…"));
      emit stateChanged();
      QTimer::singleShot(update_detail::retryDelayMs(checksums_attempts_), this,
                         [this] { requestChecksums(); });
      return;
    }
    finishDownload(false,
                   QStringLiteral("Checksum fetch failed: ") +
                       error);
    return;
  }
  const auto checksums_payload = reply->readAll();
  const auto checksums_declared_length =
      reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
  reply->deleteLater();
  // A truncated checksum list would otherwise present as "no published
  // checksum for this file", which reads as a release that was published
  // wrongly rather than as a transfer that did not finish.
  if (update_detail::isIncompleteTransfer(checksums_declared_length,
                                          checksums_payload.size())) {
    if (checksums_attempts_ < update_detail::kMaximumAttempts) {
      setStatus(QStringLiteral("Checksum list ended early; retrying…"));
      emit stateChanged();
      QTimer::singleShot(update_detail::retryDelayMs(checksums_attempts_), this,
                         [this] { requestChecksums(); });
      return;
    }
    finishDownload(false, QStringLiteral(
                              "The checksum list ended after %1 of %2 bytes; "
                              "nothing was verified.")
                              .arg(checksums_payload.size())
                              .arg(checksums_declared_length));
    return;
  }
  pending_checksums_text_ = checksums_payload;

  setStatus(QStringLiteral("Downloading update…"));
  emit stateChanged();
  requestArtifact();
}

void UpdateChecker::requestArtifact() {
  ++artifact_attempts_;
  download_progress_ = 0.0;
  auto* artifact_reply =
      network_.get(QNetworkRequest(QUrl(pending_artifact_url_)));
  connect(artifact_reply, &QNetworkReply::downloadProgress, this,
          [this](const qint64 received, const qint64 total) {
            if (total > 0) {
              download_progress_ = static_cast<double>(received) /
                                   static_cast<double>(total);
              emit stateChanged();
            }
          });
  connect(artifact_reply, &QNetworkReply::finished, this,
          [this, artifact_reply] { handleArtifactReply(artifact_reply); });
}

void UpdateChecker::handleArtifactReply(QNetworkReply* reply) {
  if (reply->error() != QNetworkReply::NoError) {
    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool retry =
        artifact_attempts_ < update_detail::kMaximumAttempts &&
        update_detail::isTransientFailure(reply->error(), status);
    const QString error = reply->errorString();
    reply->deleteLater();
    if (retry) {
      setStatus(QStringLiteral(
          "Release file is being published; retrying download…"));
      emit stateChanged();
      QTimer::singleShot(update_detail::retryDelayMs(artifact_attempts_), this,
                         [this] { requestArtifact(); });
      return;
    }
    finishDownload(false, QStringLiteral("Download failed: ") + error);
    return;
  }
  const auto file_name =
      QFileInfo(QUrl(pending_artifact_url_).path()).fileName();
  const auto payload = reply->readAll();
  const auto declared_length =
      reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
  reply->deleteLater();

  // Before hashing, not after. A transfer that stopped early hashes to
  // something that is not the published checksum, and reporting that as a
  // checksum failure accuses the release of being corrupt when the bytes
  // simply never arrived. Retried like any other transient failure, because
  // that is what it is.
  if (update_detail::isIncompleteTransfer(declared_length, payload.size())) {
    const bool retry = artifact_attempts_ < update_detail::kMaximumAttempts;
    if (retry) {
      setStatus(QStringLiteral("Download ended early; retrying…"));
      emit stateChanged();
      QTimer::singleShot(update_detail::retryDelayMs(artifact_attempts_), this,
                         [this] { requestArtifact(); });
      return;
    }
    finishDownload(false,
                   QStringLiteral(
                       "Download of %1 ended after %2 of %3 bytes. The "
                       "release was not reached; nothing was verified.")
                       .arg(file_name)
                       .arg(payload.size())
                       .arg(declared_length));
    return;
  }

  const auto expected_hash = expectedHashFor(pending_checksums_text_, file_name);
  const auto local_hash =
      QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
  if (expected_hash.isEmpty()) {
    finishDownload(false, QStringLiteral(
                              "Could not find a published checksum for %1")
                              .arg(file_name));
    return;
  }
  if (QString::fromLatin1(local_hash).compare(expected_hash,
                                              Qt::CaseInsensitive) != 0) {
    finishDownload(false, QStringLiteral(
                              "Checksum mismatch for %1 — download was "
                              "corrupted or tampered with, discarded")
                              .arg(file_name));
    return;
  }

  auto download_dir =
      QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  if (download_dir.isEmpty() || !QDir(download_dir).exists()) {
    download_dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(download_dir);
  }
  const auto save_path = QDir(download_dir).filePath(file_name);
  QFile file(save_path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
      file.write(payload) != payload.size()) {
    finishDownload(false,
                   QStringLiteral("Could not save the download to %1")
                       .arg(save_path));
    return;
  }
  file.close();
  downloaded_file_path_ = save_path;
  finishDownload(true, QStringLiteral("Downloaded and verified %1")
                            .arg(file_name));
}

void UpdateChecker::finishDownload(const bool success, QString message) {
  downloading_ = false;
  download_verified_ = success;
  if (!success) {
    download_progress_ = 0.0;
    if (!downloaded_file_path_.isEmpty()) {
      QFile::remove(downloaded_file_path_);
      downloaded_file_path_.clear();
    }
  } else {
    download_progress_ = 1.0;
  }
  setStatus(std::move(message));
  emit stateChanged();
}

QString UpdateChecker::expectedHashFor(const QByteArray& checksums_text,
                                       const QString& file_name) {
  const auto lines = QString::fromUtf8(checksums_text)
                          .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  for (const auto& line : lines) {
    const auto trimmed = line.trimmed();
    const auto separator = trimmed.indexOf(QLatin1Char(' '));
    if (separator <= 0) continue;
    const auto hash = trimmed.left(separator);
    auto name = trimmed.mid(separator).trimmed();
    if (name.startsWith(QLatin1Char('*'))) name.remove(0, 1);
    if (name == file_name) return hash;
  }
  return {};
}

void UpdateChecker::openDownloadedFile() {
  if (downloaded_file_path_.isEmpty() || !download_verified_) return;
  QDesktopServices::openUrl(QUrl::fromLocalFile(downloaded_file_path_));
}

void UpdateChecker::revealDownloadFolder() {
  if (downloaded_file_path_.isEmpty()) return;
  QDesktopServices::openUrl(
      QUrl::fromLocalFile(QFileInfo(downloaded_file_path_).absolutePath()));
}

}  // namespace cwassistant::desktop
