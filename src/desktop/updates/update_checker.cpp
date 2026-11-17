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

#include <algorithm>
#include <array>
#include <utility>

namespace cwassistant::desktop {
namespace {

constexpr auto kManifestUrl =
    "https://github.com/alessiobravi/CW-Buddy/releases/download/"
    "continuous/latest.json";

constexpr qint64 kReadChunkBytes = 64 * 1'024;

[[nodiscard]] QString userAgent() {
  return QStringLiteral("CW-Buddy/%1 (+https://github.com/alessiobravi/"
                        "CW-Buddy)")
      .arg(QCoreApplication::applicationVersion());
}

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

[[nodiscard]] QUrl redirectedUrl(QNetworkReply* reply) {
  const auto target =
      reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
  return target.isEmpty() ? QUrl{} : reply->url().resolved(target);
}

// A stalled transfer is aborted by Qt's own transfer timeout, which surfaces
// as a cancellation rather than as a timeout error. Every other cancellation
// this class can produce (the byte cap, the operator's cancel) is recognised
// before the error path runs, so by the time this is consulted a cancellation
// means the socket went quiet.
[[nodiscard]] bool isStalledTransfer(
    const QNetworkReply::NetworkError error) noexcept {
  return error == QNetworkReply::OperationCanceledError;
}

// Reads whatever the reply currently holds into a bounded buffer. Returns
// false as soon as the limit would be passed, so a server that answers a
// short text document with an endless stream is cut off instead of being
// read with readAll().
[[nodiscard]] bool drainBounded(QNetworkReply* reply, QByteArray* destination,
                                const qint64 limit) {
  while (reply->bytesAvailable() > 0) {
    const qint64 remaining = limit - destination->size();
    if (remaining <= 0) return false;
    const QByteArray chunk = reply->read(std::min(kReadChunkBytes, remaining));
    if (chunk.isEmpty()) break;
    destination->append(chunk);
    if (destination->size() >= limit && reply->bytesAvailable() > 0) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool isSafeAssetName(const QString& name) {
  if (name.contains(QStringLiteral(".."))) return false;
  static const QRegularExpression safe(
      QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
  return safe.match(name).hasMatch();
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

bool isTrustedReleaseUrl(const QUrl& manifest_url, const QUrl& candidate) {
  if (!candidate.isValid() || candidate.isRelative()) return false;
  if (!manifest_url.isValid() || manifest_url.isRelative()) return false;
  if (candidate.scheme() != QStringLiteral("https")) return false;
  if (manifest_url.scheme() != QStringLiteral("https")) return false;
  if (!candidate.userInfo().isEmpty()) return false;
  if (!manifest_url.userInfo().isEmpty()) return false;
  if (candidate.host().isEmpty() || manifest_url.host().isEmpty()) return false;
  if (candidate.host().compare(manifest_url.host(), Qt::CaseInsensitive) != 0) {
    return false;
  }
  if (candidate.port(-1) != manifest_url.port(-1)) return false;
  if (candidate.hasQuery() || candidate.hasFragment()) return false;

  const QString directory =
      manifest_url
          .adjusted(QUrl::RemoveFilename | QUrl::RemoveQuery |
                    QUrl::RemoveFragment)
          .path(QUrl::FullyEncoded);
  if (!directory.endsWith(QLatin1Char('/'))) return false;
  const QString path = candidate.path(QUrl::FullyEncoded);
  if (!path.startsWith(directory)) return false;
  // A percent-encoded separator cannot survive isSafeAssetName(), so an
  // encoded path escape is refused with everything else that is not a plain
  // asset file name sitting directly in the release directory.
  return isSafeAssetName(path.mid(directory.size()));
}

bool isTrustedRedirect(const QUrl& manifest_url, const QUrl& candidate) {
  if (!candidate.isValid() || candidate.isRelative()) return false;
  if (!manifest_url.isValid() || manifest_url.isRelative()) return false;
  if (candidate.scheme() != QStringLiteral("https")) return false;
  if (manifest_url.scheme() != QStringLiteral("https")) return false;
  if (!candidate.userInfo().isEmpty()) return false;
  if (candidate.host().isEmpty() || manifest_url.host().isEmpty()) return false;

  const QString host = candidate.host().toLower();
  const QString origin_host = manifest_url.host().toLower();
  if (host == origin_host && candidate.port(-1) == manifest_url.port(-1)) {
    return true;
  }
  const bool release_lives_on_github =
      origin_host == QStringLiteral("github.com") ||
      origin_host.endsWith(QStringLiteral(".github.com"));
  if (!release_lives_on_github) return false;
  if (candidate.port(-1) != -1 && candidate.port(-1) != 443) return false;
  return host == QStringLiteral("githubusercontent.com") ||
         host.endsWith(QStringLiteral(".githubusercontent.com"));
}

}  // namespace update_detail

UpdateChecker::UpdateChecker(QObject* parent)
    : UpdateChecker(nullptr, QUrl(QString::fromLatin1(kManifestUrl)), QString{},
                    update_detail::kMaximumArtifactBytes, parent) {}

UpdateChecker::UpdateChecker(QNetworkAccessManager* network, QUrl manifest_url,
                             QString download_directory,
                             const qint64 maximum_artifact_bytes,
                             QObject* parent)
    : QObject(parent),
      network_(network),
      manifest_url_(std::move(manifest_url)),
      download_directory_(std::move(download_directory)),
      maximum_artifact_bytes_(maximum_artifact_bytes),
      current_version_(QCoreApplication::applicationVersion()) {
  if (network_ == nullptr) network_ = new QNetworkAccessManager(this);
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

QNetworkRequest UpdateChecker::releaseRequest(const QUrl& url) const {
  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
  request.setTransferTimeout(update_detail::kTransferTimeoutMs);
  // Redirects are followed by hand so that every hop can be checked against
  // the trust rule. Qt's automatic policy would follow a cross-origin hop
  // before this class ever saw it.
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  return request;
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

QString UpdateChecker::resolveDownloadDirectory() const {
  if (!download_directory_.isEmpty()) {
    QDir().mkpath(download_directory_);
    return QDir(download_directory_).exists() ? download_directory_
                                              : QString{};
  }
  auto directory =
      QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  if (!directory.isEmpty() && QDir(directory).exists()) return directory;
  directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (directory.isEmpty()) return {};
  QDir().mkpath(directory);
  return QDir(directory).exists() ? directory : QString{};
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
  sendManifestRequest(manifest_url_, update_detail::kMaximumRedirects);
}

void UpdateChecker::sendManifestRequest(const QUrl& url,
                                        const int redirects_remaining) {
  manifest_payload_.clear();
  manifest_oversized_ = false;
  auto* reply = network_->get(releaseRequest(url));
  connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
    if (!drainBounded(reply, &manifest_payload_,
                      update_detail::kMaximumMetadataBytes)) {
      manifest_oversized_ = true;
      reply->abort();
    }
  });
  connect(reply, &QNetworkReply::finished, this,
          [this, reply, redirects_remaining] {
            handleManifestReply(reply, redirects_remaining);
          });
}

void UpdateChecker::handleManifestReply(QNetworkReply* reply,
                                        const int redirects_remaining) {
  const QUrl redirect = redirectedUrl(reply);
  if (!redirect.isEmpty()) {
    reply->deleteLater();
    if (redirects_remaining <= 0 ||
        !update_detail::isTrustedRedirect(manifest_url_, redirect)) {
      checking_ = false;
      setStatus(QStringLiteral(
          "Update check refused a redirect away from the release location."));
      emit stateChanged();
      return;
    }
    sendManifestRequest(redirect, redirects_remaining - 1);
    return;
  }
  if (manifest_oversized_) {
    reply->deleteLater();
    checking_ = false;
    setStatus(QStringLiteral(
        "Update check failed: the published manifest exceeded the %1 KiB "
        "limit and was discarded.")
                  .arg(update_detail::kMaximumMetadataBytes / 1'024));
    emit stateChanged();
    return;
  }
  if (reply->error() != QNetworkReply::NoError) {
    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool stalled = isStalledTransfer(reply->error());
    const bool retry =
        manifest_attempts_ < update_detail::kMaximumAttempts &&
        (stalled || update_detail::isTransientFailure(reply->error(), status));
    const QString error = reply->errorString();
    reply->deleteLater();
    if (retry) {
      setStatus(stalled
                    ? QStringLiteral(
                          "Update check stalled with no response; retrying…")
                    : QStringLiteral("Release is being published; retrying "
                                     "update check…"));
      emit stateChanged();
      QTimer::singleShot(update_detail::retryDelayMs(manifest_attempts_), this,
                         [this] { requestManifest(); });
      return;
    }
    checking_ = false;
    setStatus(stalled
                  ? QStringLiteral(
                        "Update check stalled for more than %1 seconds and "
                        "was abandoned.")
                        .arg(update_detail::kTransferTimeoutMs / 1'000)
                  : QStringLiteral("Update check failed: ") + error);
    emit stateChanged();
    return;
  }
  if (!update_detail::isTrustedRedirect(manifest_url_, reply->url())) {
    reply->deleteLater();
    checking_ = false;
    setStatus(QStringLiteral(
        "Update check failed: the manifest came from an unexpected origin."));
    emit stateChanged();
    return;
  }
  checking_ = false;
  if (!drainBounded(reply, &manifest_payload_,
                    update_detail::kMaximumMetadataBytes)) {
    manifest_oversized_ = true;
  }
  const auto payload = manifest_payload_;
  const bool oversized = manifest_oversized_;
  manifest_payload_.clear();
  manifest_oversized_ = false;
  reply->deleteLater();
  if (oversized) {
    setStatus(QStringLiteral(
        "Update check failed: the published manifest exceeded the %1 KiB "
        "limit and was discarded.")
                  .arg(update_detail::kMaximumMetadataBytes / 1'024));
    emit stateChanged();
    return;
  }
  const auto document = QJsonDocument::fromJson(payload);
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

  // Fail closed before a single request is made. Without this the manifest
  // chooses both the binary and the hash it is compared against, which makes
  // the SHA-256 check a transport-corruption check and nothing more.
  const QUrl artifact(artifact_url);
  const QUrl checksums(checksums_url);
  if (!update_detail::isTrustedReleaseUrl(manifest_url_, artifact) ||
      !update_detail::isTrustedReleaseUrl(manifest_url_, checksums)) {
    setStatus(QStringLiteral(
        "Update refused: the published manifest pointed outside the trusted "
        "release location (%1). Nothing was downloaded.")
                  .arg(manifest_url_.host()));
    emit stateChanged();
    return;
  }

  const QString file_name = QFileInfo(artifact.path()).fileName();
  const QString directory = resolveDownloadDirectory();
  if (directory.isEmpty()) {
    setStatus(QStringLiteral(
        "No writable download folder is available for the update."));
    emit stateChanged();
    return;
  }

  ++download_generation_;
  downloading_ = true;
  download_progress_ = 0.0;
  download_verified_ = false;
  downloaded_file_path_.clear();
  pending_artifact_url_ = artifact_url;
  pending_checksums_url_ = checksums_url;
  pending_artifact_name_ = file_name;
  pending_save_path_ = QDir(directory).filePath(file_name);
  pending_checksums_text_.clear();
  checksums_attempts_ = 0;
  artifact_attempts_ = 0;
  resetArtifactTransfer();
  setStatus(QStringLiteral("Downloading checksums…"));
  emit stateChanged();

  // Fetched sequentially, not concurrently: the artifact reply must not be
  // able to finish before pending_checksums_text_ is populated, since
  // handleArtifactReply() needs it to verify the download.
  requestChecksums();
}

void UpdateChecker::cancelDownload() {
  if (!downloading_) return;
  // The generation moves before the abort, so the cancelled reply's finished
  // handler -- which abort() may deliver synchronously -- recognises itself as
  // stale and writes nothing back.
  ++download_generation_;
  if (!active_download_reply_.isNull()) active_download_reply_->abort();
  active_download_reply_.clear();
  discardArtifactFile();
  finishDownload(false, QStringLiteral("Update download cancelled."));
}

void UpdateChecker::requestChecksums() {
  ++checksums_attempts_;
  sendChecksumsRequest(QUrl(pending_checksums_url_),
                       update_detail::kMaximumRedirects);
}

void UpdateChecker::sendChecksumsRequest(const QUrl& url,
                                         const int redirects_remaining) {
  checksums_payload_.clear();
  checksums_oversized_ = false;
  const int generation = download_generation_;
  auto* reply = network_->get(releaseRequest(url));
  active_download_reply_ = reply;
  connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
    if (!drainBounded(reply, &checksums_payload_,
                      update_detail::kMaximumMetadataBytes)) {
      checksums_oversized_ = true;
      reply->abort();
    }
  });
  connect(reply, &QNetworkReply::finished, this,
          [this, reply, redirects_remaining, generation] {
            if (generation != download_generation_) {
              reply->deleteLater();
              return;
            }
            handleChecksumsReply(reply, redirects_remaining);
          });
}

void UpdateChecker::handleChecksumsReply(QNetworkReply* reply,
                                         const int redirects_remaining) {
  const QUrl redirect = redirectedUrl(reply);
  if (!redirect.isEmpty()) {
    reply->deleteLater();
    if (redirects_remaining <= 0 ||
        !update_detail::isTrustedRedirect(manifest_url_, redirect)) {
      finishDownload(false,
                     QStringLiteral("Checksum fetch refused a redirect away "
                                    "from the release location."));
      return;
    }
    sendChecksumsRequest(redirect, redirects_remaining - 1);
    return;
  }
  if (checksums_oversized_) {
    reply->deleteLater();
    finishDownload(false,
                   QStringLiteral("The published checksum list exceeded the "
                                  "%1 KiB limit and was discarded.")
                       .arg(update_detail::kMaximumMetadataBytes / 1'024));
    return;
  }
  if (reply->error() != QNetworkReply::NoError) {
    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool stalled = isStalledTransfer(reply->error());
    const bool retry =
        checksums_attempts_ < update_detail::kMaximumAttempts &&
        (stalled || update_detail::isTransientFailure(reply->error(), status));
    const QString error = reply->errorString();
    const int generation = download_generation_;
    reply->deleteLater();
    if (retry) {
      setStatus(stalled ? QStringLiteral(
                              "Checksum fetch stalled; retrying…")
                        : QStringLiteral("Release files are being published; "
                                         "retrying checksums…"));
      emit stateChanged();
      QTimer::singleShot(update_detail::retryDelayMs(checksums_attempts_), this,
                         [this, generation] {
                           if (generation != download_generation_) return;
                           requestChecksums();
                         });
      return;
    }
    finishDownload(
        false, stalled ? QStringLiteral(
                             "Checksum fetch stalled for more than %1 seconds "
                             "and was abandoned.")
                             .arg(update_detail::kTransferTimeoutMs / 1'000)
                       : QStringLiteral("Checksum fetch failed: ") + error);
    return;
  }
  if (!update_detail::isTrustedRedirect(manifest_url_, reply->url())) {
    reply->deleteLater();
    finishDownload(false, QStringLiteral(
                              "The checksum list came from an unexpected "
                              "origin and was discarded."));
    return;
  }
  if (!drainBounded(reply, &checksums_payload_,
                    update_detail::kMaximumMetadataBytes)) {
    checksums_oversized_ = true;
  }
  const auto checksums_payload = checksums_payload_;
  const bool oversized = checksums_oversized_;
  const auto checksums_declared_length =
      reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
  checksums_payload_.clear();
  checksums_oversized_ = false;
  reply->deleteLater();
  if (oversized) {
    finishDownload(false,
                   QStringLiteral("The published checksum list exceeded the "
                                  "%1 KiB limit and was discarded.")
                       .arg(update_detail::kMaximumMetadataBytes / 1'024));
    return;
  }
  // A truncated checksum list would otherwise present as "no published
  // checksum for this file", which reads as a release that was published
  // wrongly rather than as a transfer that did not finish.
  if (update_detail::isIncompleteTransfer(checksums_declared_length,
                                          checksums_payload.size())) {
    if (checksums_attempts_ < update_detail::kMaximumAttempts) {
      setStatus(QStringLiteral("Checksum list ended early; retrying…"));
      emit stateChanged();
      const int generation = download_generation_;
      QTimer::singleShot(update_detail::retryDelayMs(checksums_attempts_), this,
                         [this, generation] {
                           if (generation != download_generation_) return;
                           requestChecksums();
                         });
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

void UpdateChecker::resetArtifactTransfer() {
  discardArtifactFile();
  artifact_hash_.reset();
  artifact_received_bytes_ = 0;
  artifact_oversized_ = false;
  artifact_write_failed_ = false;
  artifact_length_checked_ = false;
}

void UpdateChecker::discardArtifactFile() {
  if (!artifact_file_) return;
  artifact_file_->cancelWriting();
  artifact_file_.reset();
}

bool UpdateChecker::openArtifactFile() {
  if (artifact_file_) return true;
  if (pending_save_path_.isEmpty()) return false;
  // QSaveFile writes beside the destination and renames on commit, so an
  // unverified or half-transferred installer never appears at the path the
  // operator is told to open. The direct-write fallback is refused for the
  // same reason: a failure must leave the previous state alone rather than
  // truncate the target in place.
  auto file = std::make_unique<QSaveFile>(pending_save_path_);
  file->setDirectWriteFallback(false);
  if (!file->open(QIODevice::WriteOnly)) return false;
  artifact_file_ = std::move(file);
  return true;
}

bool UpdateChecker::streamArtifactChunk(QNetworkReply* reply) {
  if (!artifact_length_checked_) {
    artifact_length_checked_ = true;
    const qint64 declared =
        reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
    if (declared > maximum_artifact_bytes_) {
      artifact_oversized_ = true;
      return false;
    }
  }
  while (reply->bytesAvailable() > 0) {
    const QByteArray chunk = reply->read(kReadChunkBytes);
    if (chunk.isEmpty()) break;
    if (artifact_received_bytes_ + chunk.size() > maximum_artifact_bytes_) {
      artifact_oversized_ = true;
      return false;
    }
    if (!openArtifactFile()) {
      artifact_write_failed_ = true;
      return false;
    }
    if (artifact_file_->write(chunk) != chunk.size()) {
      artifact_write_failed_ = true;
      return false;
    }
    // Hashing and writing happen here, in 64 KiB steps spread across the
    // transfer, rather than once over the whole payload when it finishes.
    // The old shape held the entire installer in memory, hashed it in one
    // call and wrote it in one call, all in the slot that also draws the
    // waterfall; measured on a ~51 MiB artifact that was a single ~37 ms
    // stall (18 ms hash, 19 ms write). In this shape each step costs on the
    // order of 50 microseconds, peak memory is one chunk instead of the whole
    // artifact, and no worker thread is needed to get there.
    artifact_hash_.addData(chunk);
    artifact_received_bytes_ += chunk.size();
  }
  return true;
}

void UpdateChecker::requestArtifact() {
  ++artifact_attempts_;
  download_progress_ = 0.0;
  sendArtifactRequest(QUrl(pending_artifact_url_),
                      update_detail::kMaximumRedirects);
}

void UpdateChecker::sendArtifactRequest(const QUrl& url,
                                        const int redirects_remaining) {
  resetArtifactTransfer();
  const int generation = download_generation_;
  auto* artifact_reply = network_->get(releaseRequest(url));
  active_download_reply_ = artifact_reply;
  connect(artifact_reply, &QNetworkReply::readyRead, this,
          [this, artifact_reply, generation] {
            if (generation != download_generation_) return;
            if (!streamArtifactChunk(artifact_reply)) artifact_reply->abort();
          });
  connect(artifact_reply, &QNetworkReply::downloadProgress, this,
          [this, generation](const qint64 received, const qint64 total) {
            if (generation != download_generation_) return;
            if (total > 0) {
              download_progress_ = static_cast<double>(received) /
                                   static_cast<double>(total);
              emit stateChanged();
            }
          });
  connect(artifact_reply, &QNetworkReply::finished, this,
          [this, artifact_reply, redirects_remaining, generation] {
            if (generation != download_generation_) {
              artifact_reply->deleteLater();
              return;
            }
            handleArtifactReply(artifact_reply, redirects_remaining);
          });
}

void UpdateChecker::handleArtifactReply(QNetworkReply* reply,
                                        const int redirects_remaining) {
  const QUrl redirect = redirectedUrl(reply);
  if (!redirect.isEmpty()) {
    reply->deleteLater();
    discardArtifactFile();
    if (redirects_remaining <= 0 ||
        !update_detail::isTrustedRedirect(manifest_url_, redirect)) {
      finishDownload(false,
                     QStringLiteral("Update download refused a redirect away "
                                    "from the release location."));
      return;
    }
    sendArtifactRequest(redirect, redirects_remaining - 1);
    return;
  }
  // Checked before the error branch: aborting the transfer for either reason
  // sets a cancellation error that would otherwise be reported as a network
  // failure.
  if (artifact_oversized_) {
    reply->deleteLater();
    discardArtifactFile();
    finishDownload(false,
                   QStringLiteral("The update download exceeded the %1 MiB "
                                  "safety limit and was discarded.")
                       .arg(maximum_artifact_bytes_ / (1'024 * 1'024)));
    return;
  }
  if (artifact_write_failed_) {
    reply->deleteLater();
    discardArtifactFile();
    finishDownload(false, QStringLiteral("Could not save the download to %1")
                              .arg(pending_save_path_));
    return;
  }
  if (reply->error() != QNetworkReply::NoError) {
    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool stalled = isStalledTransfer(reply->error());
    const bool retry =
        artifact_attempts_ < update_detail::kMaximumAttempts &&
        (stalled || update_detail::isTransientFailure(reply->error(), status));
    const QString error = reply->errorString();
    const int generation = download_generation_;
    reply->deleteLater();
    discardArtifactFile();
    if (retry) {
      setStatus(stalled ? QStringLiteral(
                              "Download stalled with no data; retrying…")
                        : QStringLiteral("Release file is being published; "
                                         "retrying download…"));
      emit stateChanged();
      QTimer::singleShot(update_detail::retryDelayMs(artifact_attempts_), this,
                         [this, generation] {
                           if (generation != download_generation_) return;
                           requestArtifact();
                         });
      return;
    }
    finishDownload(
        false, stalled ? QStringLiteral(
                             "The download stalled with no data for more than "
                             "%1 seconds and was abandoned.")
                             .arg(update_detail::kTransferTimeoutMs / 1'000)
                       : QStringLiteral("Download failed: ") + error);
    return;
  }
  if (!update_detail::isTrustedRedirect(manifest_url_, reply->url())) {
    reply->deleteLater();
    discardArtifactFile();
    finishDownload(false, QStringLiteral(
                              "The update download came from an unexpected "
                              "origin and was discarded."));
    return;
  }

  const bool drained = streamArtifactChunk(reply);
  const auto file_name = pending_artifact_name_;
  const auto declared_length =
      reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
  const qint64 received = artifact_received_bytes_;
  reply->deleteLater();
  if (!drained) {
    discardArtifactFile();
    if (artifact_oversized_) {
      finishDownload(false,
                     QStringLiteral("The update download exceeded the %1 MiB "
                                    "safety limit and was discarded.")
                         .arg(maximum_artifact_bytes_ / (1'024 * 1'024)));
    } else {
      finishDownload(false, QStringLiteral("Could not save the download to %1")
                                .arg(pending_save_path_));
    }
    return;
  }

  // Before hashing, not after. A transfer that stopped early hashes to
  // something that is not the published checksum, and reporting that as a
  // checksum failure accuses the release of being corrupt when the bytes
  // simply never arrived. Retried like any other transient failure, because
  // that is what it is.
  if (update_detail::isIncompleteTransfer(declared_length, received)) {
    discardArtifactFile();
    const bool retry = artifact_attempts_ < update_detail::kMaximumAttempts;
    if (retry) {
      setStatus(QStringLiteral("Download ended early; retrying…"));
      emit stateChanged();
      const int generation = download_generation_;
      QTimer::singleShot(update_detail::retryDelayMs(artifact_attempts_), this,
                         [this, generation] {
                           if (generation != download_generation_) return;
                           requestArtifact();
                         });
      return;
    }
    finishDownload(false,
                   QStringLiteral(
                       "Download of %1 ended after %2 of %3 bytes. The "
                       "release was not reached; nothing was verified.")
                       .arg(file_name)
                       .arg(received)
                       .arg(declared_length));
    return;
  }

  const auto expected_hash = expectedHashFor(pending_checksums_text_, file_name);
  const auto local_hash = artifact_hash_.result().toHex();
  if (expected_hash.isEmpty()) {
    discardArtifactFile();
    finishDownload(false, QStringLiteral(
                              "Could not find a published checksum for %1")
                              .arg(file_name));
    return;
  }
  if (QString::fromLatin1(local_hash).compare(expected_hash,
                                              Qt::CaseInsensitive) != 0) {
    discardArtifactFile();
    finishDownload(false, QStringLiteral(
                              "Checksum mismatch for %1 — download was "
                              "corrupted or tampered with, discarded")
                              .arg(file_name));
    return;
  }

  // Only now does the file appear at the path the operator is handed. The
  // open is repeated rather than assumed: a verified zero-byte artifact never
  // reaches streamArtifactChunk()'s write, and must still be saved rather
  // than reported as a write failure.
  if (!openArtifactFile() || !artifact_file_->commit()) {
    discardArtifactFile();
    finishDownload(false, QStringLiteral("Could not save the download to %1")
                              .arg(pending_save_path_));
    return;
  }
  artifact_file_.reset();
  downloaded_file_path_ = pending_save_path_;
  finishDownload(true, QStringLiteral("Downloaded and verified %1")
                            .arg(file_name));
}

void UpdateChecker::finishDownload(const bool success, QString message) {
  downloading_ = false;
  download_verified_ = success;
  active_download_reply_.clear();
  if (!success) {
    discardArtifactFile();
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
