#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QUrl>

namespace cwassistant::desktop {

namespace update_detail {
inline constexpr int kMaximumAttempts = 3;

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
  void requestChecksums();
  void requestArtifact();
  void handleManifestReply(QNetworkReply* reply);
  void handleChecksumsReply(QNetworkReply* reply);
  void handleArtifactReply(QNetworkReply* reply);
  void setStatus(QString message);
  void finishDownload(bool success, QString message);
  [[nodiscard]] static QString platformArtifactKey();
  [[nodiscard]] static QString expectedHashFor(const QByteArray& checksums_text,
                                               const QString& file_name);

  QNetworkAccessManager network_;
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
  QByteArray pending_checksums_text_;
  int manifest_attempts_{0};
  int checksums_attempts_{0};
  int artifact_attempts_{0};
};

}  // namespace cwassistant::desktop
