#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QSaveFile>
#include <QString>
#include <QUrl>

#include <memory>

namespace cwassistant::desktop {

namespace update_detail {
inline constexpr int kMaximumAttempts = 3;
// A redirect chain longer than this is a loop or a misconfiguration, not a
// delivery path. GitHub uses exactly one hop from the release URL to the
// signed asset host.
inline constexpr int kMaximumRedirects = 4;
// Matches the sibling callsign-database updater. Twenty seconds is long
// enough for a slow link to make progress and short enough that a stalled
// transfer clears the downloading state while the operator is still looking
// at the dialog.
inline constexpr int kTransferTimeoutMs = 20'000;
// The largest artifact this release channel publishes today is the Windows
// MSI at roughly 51 MiB, and the whole five-platform asset set is under
// 250 MiB. The cap has to clear a single artifact's plausible growth --
// bundling the optional ONNX Runtime backend and a Qt major-version update is
// the realistic path and would roughly triple it, to around 150 MiB -- while
// staying below the size of the entire release, so a server that starts
// streaming something that is not an installer (a repository tarball, a disk
// image, an endless stream) is cut off long before it fills a small Downloads
// partition. 256 MiB sits above the first figure and below the second.
inline constexpr qint64 kMaximumArtifactBytes = 256LL * 1'024LL * 1'024LL;
// Both documents fetched before the artifact have a fixed, tiny shape: the
// manifest is a five-artifact JSON object under 1 KiB, and SHA256SUMS is one
// ~100-byte line per artifact. 64 KiB allows roughly 650 checksum lines or an
// eighty-fold larger manifest and refuses anything that is not a short text
// document. The sibling updater allows 1 MiB because its provider returns an
// open-ended file listing; nothing here is open-ended.
inline constexpr qint64 kMaximumMetadataBytes = 64LL * 1'024LL;

struct UpdateActionVisibility {
  bool row{false};
  bool download{false};
  bool verified_download{false};
};

[[nodiscard]] bool isTransientFailure(QNetworkReply::NetworkError error,
                                      int http_status) noexcept;
[[nodiscard]] int retryDelayMs(int completed_attempts) noexcept;
// Whether a transfer that reported no error nonetheless ended short of what
// the server said it would send.
//
// A download that stops early is not always an error the network stack
// reports: a gateway that times out mid-body, or a connection closed after the
// headers, can leave a short payload behind a perfectly successful reply. The
// bytes are then hashed, the hash does not match, and the operator is told the
// download was corrupted or tampered with -- which sends them looking for a
// bad release when the release is intact and the transfer simply did not
// finish. The two failures need different answers: a short transfer is worth
// retrying, and a complete transfer whose contents are wrong is not.
//
// A declared length of zero or less means the server did not say, which is not
// evidence of anything; only a payload demonstrably shorter than an announced
// length counts.
[[nodiscard]] bool isIncompleteTransfer(qint64 declared_content_length,
                                        qint64 received_bytes) noexcept;
[[nodiscard]] UpdateActionVisibility updateActionVisibility(
    bool update_available, bool platform_supported,
    bool download_verified) noexcept;

// The trust rule for every URL the published manifest supplies.
//
// The manifest itself is fetched from a URL compiled into this build, so it is
// the only thing in the chain that is pinned by construction. Everything the
// manifest then names -- the artifact to download and the checksum list it is
// compared against -- must sit in the manifest's own directory on the
// manifest's own origin, under a plain asset file name.
//
// Same-origin alone would not be enough. github.com is multi-tenant: anyone
// can publish a release and serve an arbitrary installer from
// https://github.com/<someone-else>/<repo>/releases/download/..., which is the
// same scheme, host and port as this project's manifest. Requiring the same
// directory prefix narrows the trusted set to the assets of this project's own
// continuous release, which is exactly the set the publishing workflow writes.
//
// Refuses, specifically: any other host or port, http and every other scheme,
// embedded credentials, a query string or fragment, a path outside the
// manifest's directory, a nested path below it, "." and ".." segments,
// percent-encoded separators, and empty or exotic file names.
[[nodiscard]] bool isTrustedReleaseUrl(const QUrl& manifest_url,
                                       const QUrl& candidate);
// The trust rule for a redirect target.
//
// Deliberately looser than isTrustedReleaseUrl, because a redirect is not
// chosen by the manifest: the request that produced it was already pinned, so
// the redirect comes from the pinned origin over authenticated TLS. Its job is
// to refuse a scheme downgrade, embedded credentials, and an open redirect
// bouncing the download to an unrelated third party.
//
// GitHub answers a release-asset URL with a redirect to a separate signed
// delivery host, and that host name has changed more than once over this
// project's life, so the allowance is for the delivery domain rather than one
// hard-coded host -- and only when the release being fetched is itself on
// GitHub. Integrity does not rest on it: the bytes are still compared against
// a SHA-256 taken from a checksum list fetched under the strict rule above.
[[nodiscard]] bool isTrustedRedirect(const QUrl& manifest_url,
                                     const QUrl& candidate);
}  // namespace update_detail

// Operator-controlled update checking, download, and checksum verification
// against the published continuous-release manifest (PKG-004). This class
// never installs or replaces anything itself: once a download is verified,
// the operator explicitly opens it with the OS's own installer/package
// handler (a "guided" install, not a silent self-update) or reveals it in
// the file manager to finish manually. Nothing is downloaded or opened
// without an explicit Q_INVOKABLE call from the operator-facing UI.
class UpdateChecker final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool autoCheckEnabled READ autoCheckEnabled
                 WRITE setAutoCheckEnabled NOTIFY autoCheckEnabledChanged)
  Q_PROPERTY(bool checking READ checking NOTIFY stateChanged)
  Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY stateChanged)
  Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
  Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY stateChanged)
  Q_PROPERTY(QString lastCheckedText READ lastCheckedText NOTIFY stateChanged)
  Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
  Q_PROPERTY(bool downloading READ downloading NOTIFY stateChanged)
  Q_PROPERTY(double downloadProgress READ downloadProgress NOTIFY stateChanged)
  Q_PROPERTY(bool downloadVerified READ downloadVerified NOTIFY stateChanged)
  Q_PROPERTY(QString downloadedFilePath READ downloadedFilePath
                 NOTIFY stateChanged)
  Q_PROPERTY(bool platformSupported READ platformSupported CONSTANT)
  Q_PROPERTY(bool updateActionVisible READ updateActionVisible
                 NOTIFY stateChanged)
  Q_PROPERTY(bool downloadActionVisible READ downloadActionVisible
                 NOTIFY stateChanged)
  Q_PROPERTY(bool verifiedDownloadActionsVisible
                 READ verifiedDownloadActionsVisible NOTIFY stateChanged)

 public:
  explicit UpdateChecker(QObject* parent = nullptr);
  // The injected manager/manifest constructor exists for deterministic tests.
  // Production uses the compiled-in HTTPS manifest URL through the constructor
  // above. The injected manager must outlive this object. An empty
  // download_directory keeps the production Downloads-folder resolution.
  UpdateChecker(QNetworkAccessManager* network, QUrl manifest_url,
                QString download_directory,
                qint64 maximum_artifact_bytes =
                    update_detail::kMaximumArtifactBytes,
                QObject* parent = nullptr);

  [[nodiscard]] bool autoCheckEnabled() const noexcept;
  void setAutoCheckEnabled(bool value);
  [[nodiscard]] bool checking() const noexcept;
  [[nodiscard]] bool updateAvailable() const noexcept;
  [[nodiscard]] const QString& currentVersion() const noexcept;
  [[nodiscard]] const QString& latestVersion() const noexcept;
  [[nodiscard]] QString lastCheckedText() const;
  [[nodiscard]] const QString& statusMessage() const noexcept;
  [[nodiscard]] bool downloading() const noexcept;
  [[nodiscard]] double downloadProgress() const noexcept;
  [[nodiscard]] bool downloadVerified() const noexcept;
  [[nodiscard]] const QString& downloadedFilePath() const noexcept;
  [[nodiscard]] bool platformSupported() const noexcept;
  [[nodiscard]] bool updateActionVisible() const noexcept;
  [[nodiscard]] bool downloadActionVisible() const noexcept;
  [[nodiscard]] bool verifiedDownloadActionsVisible() const noexcept;

  // Fetches the published manifest and compares its version to this build.
  Q_INVOKABLE void checkForUpdates();
  // Downloads this platform's artifact (found in the last successfully
  // fetched manifest) to the user's Downloads folder and verifies its
  // SHA-256 against the published SHA256SUMS before reporting success.
  Q_INVOKABLE void downloadUpdate();
  // Abandons a download in progress, including one waiting on a retry timer,
  // and discards the partial file. Without it a transfer that never completes
  // would leave downloading() true for the rest of the session, which both
  // the startup notice and Settings -> About use to disable their download
  // action. The transfer timeout covers a silently stalled socket; this covers
  // the operator deciding not to wait.
  Q_INVOKABLE void cancelDownload();
  // Opens the verified download with the OS's default handler (the MSI/deb
  // installer UI, or an archive tool for the portable builds) so the
  // operator completes the install themselves.
  Q_INVOKABLE void openDownloadedFile();
  // Reveals the verified download in the OS file manager as a fallback if
  // the default handler is not what the operator wants.
  Q_INVOKABLE void revealDownloadFolder();

 signals:
  void stateChanged();
  void autoCheckEnabledChanged();

 private:
  void requestManifest();
  void sendManifestRequest(const QUrl& url, int redirects_remaining);
  void requestChecksums();
  void sendChecksumsRequest(const QUrl& url, int redirects_remaining);
  void requestArtifact();
  void sendArtifactRequest(const QUrl& url, int redirects_remaining);
  void handleManifestReply(QNetworkReply* reply, int redirects_remaining);
  void handleChecksumsReply(QNetworkReply* reply, int redirects_remaining);
  void handleArtifactReply(QNetworkReply* reply, int redirects_remaining);
  [[nodiscard]] QNetworkRequest releaseRequest(const QUrl& url) const;
  // Reads whatever the reply currently holds into the artifact file, hashing
  // as it goes. Returns false once the byte cap or a write failure ends the
  // transfer; the reason is left in artifact_oversized_/artifact_write_failed_.
  [[nodiscard]] bool streamArtifactChunk(QNetworkReply* reply);
  [[nodiscard]] bool openArtifactFile();
  void discardArtifactFile();
  void resetArtifactTransfer();
  [[nodiscard]] QString resolveDownloadDirectory() const;
  void setStatus(QString message);
  void finishDownload(bool success, QString message);
  [[nodiscard]] static QString platformArtifactKey();
  [[nodiscard]] static QString expectedHashFor(const QByteArray& checksums_text,
                                               const QString& file_name);

  QNetworkAccessManager* network_{nullptr};
  QUrl manifest_url_;
  QString download_directory_;
  qint64 maximum_artifact_bytes_{update_detail::kMaximumArtifactBytes};
  QString current_version_;
  QString latest_version_;
  QString status_message_{QStringLiteral("Not checked yet")};
  QDateTime last_checked_;
  bool auto_check_enabled_{true};
  bool checking_{false};
  bool update_available_{false};
  bool downloading_{false};
  double download_progress_{0.0};
  bool download_verified_{false};
  QString downloaded_file_path_;
  QJsonObject last_manifest_;
  QString pending_artifact_url_;
  QString pending_checksums_url_;
  QString pending_artifact_name_;
  QString pending_save_path_;
  QByteArray pending_checksums_text_;
  QByteArray manifest_payload_;
  QByteArray checksums_payload_;
  bool manifest_oversized_{false};
  bool checksums_oversized_{false};
  int manifest_attempts_{0};
  int checksums_attempts_{0};
  int artifact_attempts_{0};
  // Bumped by every downloadUpdate() and cancelDownload(). Replies and retry
  // timers carry the generation they were started for and do nothing when it
  // no longer matches, so an aborted transfer that finishes late cannot write
  // state back over a cancelled or restarted download.
  int download_generation_{0};
  // Streaming artifact state. The payload is never held whole in memory and
  // never appears at its final path unverified.
  std::unique_ptr<QSaveFile> artifact_file_;
  QCryptographicHash artifact_hash_{QCryptographicHash::Sha256};
  qint64 artifact_received_bytes_{0};
  bool artifact_oversized_{false};
  bool artifact_write_failed_{false};
  bool artifact_length_checked_{false};
  QPointer<QNetworkReply> active_download_reply_;
};

}  // namespace cwassistant::desktop
