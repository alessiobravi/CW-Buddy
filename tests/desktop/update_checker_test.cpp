// Deterministic update-checker tests. Every network reply is scripted through
// an overridden QNetworkAccessManager::createRequest, exactly as
// callsign_database_updater_test.cpp does, so nothing here opens a socket,
// resolves a name, or reaches github.com. The URLs below are strings the
// trust rule is asked about, never addresses anything connects to; a global
// assertion at the end of main() proves no request was ever issued to one of
// the hostile hosts the manifest fixtures name.
#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "updates/update_checker.hpp"

namespace {

using cwassistant::desktop::UpdateChecker;
namespace detail = cwassistant::desktop::update_detail;

constexpr auto kManifestUrl =
    "https://github.com/alessiobravi/CW-Buddy/releases/download/continuous/"
    "latest.json";
constexpr auto kReleaseDirectory =
    "https://github.com/alessiobravi/CW-Buddy/releases/download/continuous/";
constexpr auto kArtifactName = "cw-buddy-update.bin";

int failures = 0;

void expect(const bool condition, const char* message) {
  if (condition) return;
  ++failures;
  std::cerr << "FAIL: " << message << '\n';
}

// ---------------------------------------------------------------------------
// Scripted network layer
// ---------------------------------------------------------------------------

struct ScriptedResponse {
  int status{200};
  QByteArray body;
  QNetworkReply::NetworkError error{QNetworkReply::NoError};
  QString error_text;
  QUrl redirect;
  // -1 announces the body's real size; -2 announces nothing; anything else is
  // announced verbatim, which is how a short transfer and an over-cap
  // announcement are simulated without moving those bytes.
  qint64 declared_length{-1};
  bool never_finishes{false};
};

class ScriptedReply final : public QNetworkReply {
 public:
  ScriptedReply(const QNetworkAccessManager::Operation operation,
                const QNetworkRequest& request, ScriptedResponse response,
                QObject* parent)
      : QNetworkReply(parent), body_(std::move(response.body)) {
    setOperation(operation);
    setRequest(request);
    setUrl(request.url());
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.status);
    if (response.declared_length != -2) {
      setHeader(QNetworkRequest::ContentLengthHeader,
                response.declared_length == -1
                    ? static_cast<qint64>(body_.size())
                    : response.declared_length);
    }
    if (!response.redirect.isEmpty()) {
      setAttribute(QNetworkRequest::RedirectionTargetAttribute,
                   response.redirect);
    }
    if (response.error != QNetworkReply::NoError) {
      setError(response.error, response.error_text.isEmpty()
                                   ? QStringLiteral("scripted network error")
                                   : response.error_text);
    }
    open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    if (response.never_finishes) return;
    QTimer::singleShot(0, this, [this] {
      if (!body_.isEmpty()) emit readyRead();
      if (isFinished()) return;
      setFinished(true);
      emit finished();
    });
  }

  void abort() override {
    if (isFinished()) return;
    setError(QNetworkReply::OperationCanceledError,
             QStringLiteral("scripted request aborted"));
    setFinished(true);
    QTimer::singleShot(0, this, [this] { emit finished(); });
  }

  [[nodiscard]] qint64 bytesAvailable() const override {
    return body_.size() - offset_ + QNetworkReply::bytesAvailable();
  }

 protected:
  qint64 readData(char* destination, const qint64 maximum) override {
    if (offset_ >= body_.size()) return -1;
    const qint64 count = std::min(maximum, body_.size() - offset_);
    std::memcpy(destination, body_.constData() + offset_,
                static_cast<std::size_t>(count));
    offset_ += count;
    return count;
  }

 private:
  QByteArray body_;
  qint64 offset_{0};
};

class ScriptedNetworkAccessManager final : public QNetworkAccessManager {
 public:
  struct ObservedRequest {
    Operation operation;
    QNetworkRequest request;
  };

  void enqueue(ScriptedResponse response) {
    responses_.push_back(std::move(response));
  }

  [[nodiscard]] const std::vector<ObservedRequest>& requests() const noexcept {
    return requests_;
  }
  [[nodiscard]] int unexpectedRequests() const noexcept {
    return unexpected_requests_;
  }
  [[nodiscard]] std::size_t count() const noexcept { return requests_.size(); }
  [[nodiscard]] QUrl url(const std::size_t index) const {
    return index < requests_.size() ? requests_[index].request.url() : QUrl{};
  }

 protected:
  QNetworkReply* createRequest(Operation operation,
                               const QNetworkRequest& request,
                               QIODevice*) override {
    requests_.push_back({operation, request});
    ScriptedResponse response;
    if (responses_.empty()) {
      ++unexpected_requests_;
      response.status = 0;
      response.error = QNetworkReply::ProtocolUnknownError;
      response.error_text = QStringLiteral("no scripted response");
    } else {
      response = std::move(responses_.front());
      responses_.pop_front();
    }
    return new ScriptedReply(operation, request, std::move(response), this);
  }

 private:
  std::deque<ScriptedResponse> responses_;
  std::vector<ObservedRequest> requests_;
  int unexpected_requests_{0};
};

// Every request any case in this file issues, so one assertion at the end can
// prove nothing was ever sent to a host the fixtures only name as bait.
std::vector<QUrl> all_requested_urls;

void recordRequests(const ScriptedNetworkAccessManager& network) {
  for (const auto& observed : network.requests()) {
    all_requested_urls.push_back(observed.request.url());
  }
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

bool waitUntil(const std::function<bool()>& predicate,
               const int timeout_ms = 2'000) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < timeout_ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(1);
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  return predicate();
}

QString sha256(const QByteArray& payload) {
  return QString::fromLatin1(
      QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
}

QByteArray checksumList(const QByteArray& payload,
                        const QString& name = QString::fromLatin1(
                            kArtifactName)) {
  return QStringLiteral("%1  %2\n%3  unrelated-asset.txt\n")
      .arg(sha256(payload), name, QString(64, QLatin1Char('b')))
      .toUtf8();
}

// A manifest whose five platform keys all name one artifact, so the fixture
// does not have to know which platform is running the test.
QByteArray manifestJson(const QString& artifact_url,
                        const QString& checksums_url) {
  static const char* keys[] = {"windows11-x64-installer", "debian-ubuntu-x64",
                               "linux-x64-portable", "macos-arm64",
                               "macos-x64"};
  QString artifacts;
  for (const char* key : keys) {
    if (!artifacts.isEmpty()) artifacts += QStringLiteral(",");
    artifacts += QStringLiteral("\"%1\":\"%2\"")
                     .arg(QString::fromLatin1(key), artifact_url);
  }
  return QStringLiteral(
             R"({"schemaVersion":1,"version":"9.9.9","checksums":"%1","artifacts":{%2}})")
      .arg(checksums_url, artifacts)
      .toUtf8();
}

void clearSettings() {
  QSettings settings;
  settings.clear();
  settings.sync();
}

QString pinned(const char* leaf) {
  return QString::fromLatin1(kReleaseDirectory) + QString::fromLatin1(leaf);
}

QString artifactPath(const QString& directory) {
  return QDir(directory).filePath(QString::fromLatin1(kArtifactName));
}

// Runs checkForUpdates() against a scripted manifest and leaves the updater
// holding it, which is the precondition downloadUpdate() enforces.
std::unique_ptr<UpdateChecker> primedChecker(
    ScriptedNetworkAccessManager* network, const QString& directory,
    const QString& artifact_url, const QString& checksums_url,
    const qint64 cap = detail::kMaximumArtifactBytes) {
  clearSettings();
  // Every case starts from an empty download folder, so "nothing was written"
  // assertions cannot be satisfied or defeated by what a previous case left.
  QFile::remove(artifactPath(directory));
  network->enqueue({.status = 200,
                    .body = manifestJson(artifact_url, checksums_url)});
  auto checker = std::make_unique<UpdateChecker>(
      network, QUrl(QString::fromLatin1(kManifestUrl)), directory, cap,
      nullptr);
  checker->checkForUpdates();
  expect(waitUntil([&] { return !checker->checking(); }),
         "scripted manifest check completes");
  expect(checker->updateAvailable(),
         "scripted manifest reports an available update");
  return checker;
}

// ---------------------------------------------------------------------------
// 1. The trust rule itself
// ---------------------------------------------------------------------------

void testTrustRule() {
  const QUrl manifest(QString::fromLatin1(kManifestUrl));

  expect(detail::isTrustedReleaseUrl(manifest, QUrl(pinned("SHA256SUMS"))),
         "trust rule accepts the published checksum list");
  expect(detail::isTrustedReleaseUrl(
             manifest, QUrl(pinned("cw-buddy-windows11-x64.msi"))),
         "trust rule accepts a published platform artifact");
  expect(detail::isTrustedReleaseUrl(manifest, manifest),
         "trust rule accepts the manifest's own URL");

  // github.com is multi-tenant: same scheme, host and port is not the same
  // publisher. This is the case a plain same-origin rule would wave through.
  expect(!detail::isTrustedReleaseUrl(
             manifest,
             QUrl(QStringLiteral("https://github.com/attacker/CW-Buddy/"
                                 "releases/download/continuous/evil.msi"))),
         "trust rule refuses another repository on the same origin");
  expect(!detail::isTrustedReleaseUrl(
             manifest, QUrl(QStringLiteral("https://evil.test/evil.msi"))),
         "trust rule refuses another host");
  expect(!detail::isTrustedReleaseUrl(
             manifest, QUrl(QStringLiteral(
                           "https://github.com.evil.test/alessiobravi/"
                           "CW-Buddy/releases/download/continuous/evil.msi"))),
         "trust rule refuses a host that merely starts with the real one");
  expect(!detail::isTrustedReleaseUrl(
             manifest,
             QUrl(QStringLiteral("http://github.com/alessiobravi/CW-Buddy/"
                                 "releases/download/continuous/a.msi"))),
         "trust rule refuses a scheme downgrade");
  expect(!detail::isTrustedReleaseUrl(
             manifest,
             QUrl(QStringLiteral("https://github.com:8443/alessiobravi/"
                                 "CW-Buddy/releases/download/continuous/"
                                 "a.msi"))),
         "trust rule refuses another port");
  expect(!detail::isTrustedReleaseUrl(
             manifest, QUrl(QStringLiteral(
                           "https://operator:secret@github.com/alessiobravi/"
                           "CW-Buddy/releases/download/continuous/a.msi"))),
         "trust rule refuses embedded credentials");
  expect(!detail::isTrustedReleaseUrl(manifest,
                                      QUrl(pinned("a.msi?redirect=evil"))),
         "trust rule refuses a query string");
  expect(!detail::isTrustedReleaseUrl(manifest, QUrl(pinned("sub/a.msi"))),
         "trust rule refuses a nested path below the release directory");
  expect(!detail::isTrustedReleaseUrl(manifest,
                                      QUrl(pinned("../../../../evil.msi"))),
         "trust rule refuses a traversal out of the release directory");
  expect(!detail::isTrustedReleaseUrl(manifest, QUrl(pinned("%2e%2e%2fevil"))),
         "trust rule refuses a percent-encoded traversal");
  expect(!detail::isTrustedReleaseUrl(manifest, QUrl(pinned("a%2Fb"))),
         "trust rule refuses a percent-encoded separator");
  expect(!detail::isTrustedReleaseUrl(manifest, QUrl(pinned(""))),
         "trust rule refuses an empty asset name");
  expect(!detail::isTrustedReleaseUrl(manifest, QUrl(pinned(".hidden"))),
         "trust rule refuses a dot-leading asset name");
  expect(!detail::isTrustedReleaseUrl(
             manifest, QUrl(QStringLiteral("file:///etc/passwd"))),
         "trust rule refuses a non-http scheme");
  expect(!detail::isTrustedReleaseUrl(manifest, QUrl(QStringLiteral("a.msi"))),
         "trust rule refuses a relative URL");
  // A manifest that is not itself pinned to HTTPS can vouch for nothing.
  expect(!detail::isTrustedReleaseUrl(
             QUrl(QStringLiteral("http://github.com/a/b/latest.json")),
             QUrl(QStringLiteral("https://github.com/a/b/x.msi"))),
         "trust rule refuses to work from a non-HTTPS manifest");
}

void testRedirectRule() {
  const QUrl manifest(QString::fromLatin1(kManifestUrl));

  expect(detail::isTrustedRedirect(manifest, QUrl(pinned("a.msi"))),
         "redirect rule accepts a hop within the release origin");
  expect(detail::isTrustedRedirect(
             manifest,
             QUrl(QStringLiteral("https://objects.githubusercontent.com/"
                                 "github-production-release-asset/1/2?"
                                 "X-Amz-Signature=abc"))),
         "redirect rule accepts GitHub's signed asset delivery host");
  expect(detail::isTrustedRedirect(
             manifest, QUrl(QStringLiteral(
                           "https://release-assets.githubusercontent.com/x"))),
         "redirect rule accepts a renamed GitHub delivery host");
  expect(!detail::isTrustedRedirect(
             manifest,
             QUrl(QStringLiteral("http://objects.githubusercontent.com/x"))),
         "redirect rule refuses a scheme downgrade on delivery");
  expect(!detail::isTrustedRedirect(
             manifest, QUrl(QStringLiteral("https://evil.test/x"))),
         "redirect rule refuses an unrelated host");
  expect(!detail::isTrustedRedirect(
             manifest, QUrl(QStringLiteral(
                           "https://githubusercontent.com.evil.test/x"))),
         "redirect rule refuses a delivery-host lookalike");
  expect(!detail::isTrustedRedirect(
             manifest, QUrl(QStringLiteral(
                           "https://o:p@objects.githubusercontent.com/x"))),
         "redirect rule refuses embedded credentials");
  // The GitHub delivery allowance is tied to the release living on GitHub.
  const QUrl other(QStringLiteral("https://releases.test/d/latest.json"));
  expect(detail::isTrustedRedirect(
             other, QUrl(QStringLiteral("https://releases.test/elsewhere"))),
         "redirect rule accepts same-origin hops for any release origin");
  expect(!detail::isTrustedRedirect(
             other,
             QUrl(QStringLiteral("https://objects.githubusercontent.com/x"))),
         "redirect rule does not lend GitHub delivery to another origin");
}

// ---------------------------------------------------------------------------
// 2. End-to-end behaviour
// ---------------------------------------------------------------------------

void testVerifiedDownloadAndRequestHardening(const QString& directory) {
  const QByteArray payload("CW Buddy scripted installer payload\n");
  ScriptedNetworkAccessManager network;
  auto checker =
      primedChecker(&network, directory, pinned(kArtifactName),
                    pinned("SHA256SUMS"));
  network.enqueue({.status = 200, .body = checksumList(payload)});
  network.enqueue({.status = 200, .body = payload});
  checker->downloadUpdate();
  expect(waitUntil([&] { return !checker->downloading(); }),
         "scripted verified download completes");

  const QString saved = artifactPath(directory);
  expect(checker->downloadVerified(), "verified download reports success");
  expect(checker->downloadedFilePath() == saved,
         "verified download exposes the saved path");
  expect(checker->statusMessage().contains(QStringLiteral("verified")),
         "verified download keeps a visible verification result");
  expect(checker->verifiedDownloadActionsVisible() &&
             !checker->downloadActionVisible(),
         "verified download swaps the download action for the open actions");
  QFile file(saved);
  expect(file.open(QIODevice::ReadOnly) && file.readAll() == payload,
         "verified download writes exactly the published bytes");
  file.close();
  expect(network.count() == 3 && network.url(1) == QUrl(pinned("SHA256SUMS")) &&
             network.url(2) == QUrl(pinned(kArtifactName)),
         "download uses only the manifest's pinned checksum and artifact URLs");
  expect(network.unexpectedRequests() == 0,
         "verified download performs no unscripted request");

  // Every request carries the stall timeout and takes redirects by hand.
  // The redirect policy is checked for having been set at all, not only for
  // its value: QNetworkRequest::ManualRedirectPolicy is zero, so an unset
  // attribute reads back as the safe answer and would let the default
  // automatic policy through unnoticed.
  bool timeouts_set = true;
  bool manual_redirects_set = true;
  for (const auto& observed : network.requests()) {
    if (observed.request.transferTimeout() != detail::kTransferTimeoutMs) {
      timeouts_set = false;
    }
    const QVariant policy =
        observed.request.attribute(QNetworkRequest::RedirectPolicyAttribute);
    if (!policy.isValid() ||
        policy.toInt() !=
            static_cast<int>(QNetworkRequest::ManualRedirectPolicy)) {
      manual_redirects_set = false;
    }
  }
  expect(timeouts_set, "every update request sets a transfer timeout");
  expect(manual_redirects_set,
         "every update request takes redirects by hand");
  recordRequests(network);
  QFile::remove(saved);
}

void testUntrustedManifestUrlsNeverReachTheNetwork(const QString& directory) {
  struct Case {
    const char* artifact;
    const char* checksums;
    const char* message;
  };
  const Case cases[] = {
      {"https://evil.test/cw-buddy-update.bin",
       "https://github.com/alessiobravi/CW-Buddy/releases/download/continuous/"
       "SHA256SUMS",
       "an off-origin artifact URL is refused before any request"},
      {"https://github.com/alessiobravi/CW-Buddy/releases/download/continuous/"
       "cw-buddy-update.bin",
       "https://evil.test/SHA256SUMS",
       "an off-origin checksum URL is refused before any request"},
      {"https://github.com/attacker/CW-Buddy/releases/download/continuous/"
       "cw-buddy-update.bin",
       "https://github.com/alessiobravi/CW-Buddy/releases/download/continuous/"
       "SHA256SUMS",
       "a same-origin artifact from another repository is refused"},
      {"http://github.com/alessiobravi/CW-Buddy/releases/download/continuous/"
       "cw-buddy-update.bin",
       "https://github.com/alessiobravi/CW-Buddy/releases/download/continuous/"
       "SHA256SUMS",
       "a downgraded artifact URL is refused"},
  };

  for (const auto& test_case : cases) {
    ScriptedNetworkAccessManager network;
    auto checker = primedChecker(&network, directory,
                                 QString::fromLatin1(test_case.artifact),
                                 QString::fromLatin1(test_case.checksums));
    checker->downloadUpdate();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    expect(network.count() == 1, test_case.message);
    expect(!checker->downloading() && !checker->downloadVerified(),
           "a refused manifest URL leaves no download in progress");
    expect(checker->statusMessage().contains(
               QStringLiteral("trusted release location")),
           "a refused manifest URL says so to the operator");
    recordRequests(network);
  }
}

void testRedirectsAreCheckedOnEveryHop(const QString& directory) {
  // A checksum list that redirects off-origin is refused, and the download
  // stops there rather than carrying on with whatever the hop returned.
  {
    ScriptedNetworkAccessManager network;
    auto checker = primedChecker(&network, directory, pinned(kArtifactName),
                                 pinned("SHA256SUMS"));
    network.enqueue(
        {.status = 302,
         .redirect = QUrl(QStringLiteral("https://evil.test/SHA256SUMS"))});
    checker->downloadUpdate();
    expect(waitUntil([&] { return !checker->downloading(); }),
           "an off-origin checksum redirect settles");
    expect(network.count() == 2,
           "an off-origin checksum redirect is never followed");
    expect(checker->statusMessage().contains(QStringLiteral("redirect")),
           "an off-origin checksum redirect is reported as a redirect");
    recordRequests(network);
  }

  // The same for the artifact itself.
  {
    const QByteArray payload("payload\n");
    ScriptedNetworkAccessManager network;
    auto checker = primedChecker(&network, directory, pinned(kArtifactName),
                                 pinned("SHA256SUMS"));
    network.enqueue({.status = 200, .body = checksumList(payload)});
    network.enqueue({.status = 302,
                     .redirect = QUrl(QStringLiteral(
                         "https://evil.test/cw-buddy-update.bin"))});
    checker->downloadUpdate();
    expect(waitUntil([&] { return !checker->downloading(); }),
           "an off-origin artifact redirect settles");
    expect(network.count() == 3,
           "an off-origin artifact redirect is never followed");
    expect(checker->statusMessage().contains(QStringLiteral("redirect")) &&
               !checker->downloadVerified(),
           "an off-origin artifact redirect fails closed");
    expect(!QFile::exists(
               artifactPath(directory)),
           "a refused redirect leaves nothing at the download path");
    recordRequests(network);
  }

  // GitHub's own signed delivery host is still reachable, so pinning the
  // manifest origin does not break the real download path.
  {
    const QByteArray payload("delivered by the signed asset host\n");
    const QUrl delivery(QStringLiteral(
        "https://objects.githubusercontent.com/release-asset/1?sig=abc"));
    ScriptedNetworkAccessManager network;
    auto checker = primedChecker(&network, directory, pinned(kArtifactName),
                                 pinned("SHA256SUMS"));
    network.enqueue({.status = 200, .body = checksumList(payload)});
    network.enqueue({.status = 302, .redirect = delivery});
    network.enqueue({.status = 200, .body = payload});
    checker->downloadUpdate();
    expect(waitUntil([&] { return !checker->downloading(); }),
           "a GitHub delivery redirect settles");
    expect(checker->downloadVerified() && network.count() == 4 &&
               network.url(3) == delivery,
           "a GitHub delivery redirect is followed and verifies");
    recordRequests(network);
    QFile::remove(artifactPath(directory));
  }

  // The manifest fetch is pinned on the same terms.
  {
    clearSettings();
    ScriptedNetworkAccessManager network;
    network.enqueue(
        {.status = 302,
         .redirect = QUrl(QStringLiteral("https://evil.test/latest.json"))});
    UpdateChecker checker(&network, QUrl(QString::fromLatin1(kManifestUrl)),
                          directory, detail::kMaximumArtifactBytes, nullptr);
    checker.checkForUpdates();
    expect(waitUntil([&] { return !checker.checking(); }),
           "an off-origin manifest redirect settles");
    expect(network.count() == 1 &&
               checker.statusMessage().contains(QStringLiteral("redirect")),
           "an off-origin manifest redirect is never followed");
    recordRequests(network);
  }
}

void testSizeCaps(const QString& directory) {
  // An announced length over the production cap is refused before the bytes
  // are moved, and is not mistaken for a transfer that ended early.
  {
    ScriptedNetworkAccessManager network;
    auto checker = primedChecker(&network, directory, pinned(kArtifactName),
                                 pinned("SHA256SUMS"));
    network.enqueue({.status = 200, .body = checksumList(QByteArray("x"))});
    network.enqueue({.status = 200,
                     .body = QByteArray("x"),
                     .declared_length = detail::kMaximumArtifactBytes + 1});
    checker->downloadUpdate();
    expect(waitUntil([&] { return !checker->downloading(); }),
           "an over-cap announced length settles");
    expect(network.count() == 3,
           "an over-cap announced length is not retried as a short transfer");
    expect(checker->statusMessage().contains(QStringLiteral("256 MiB")),
           "an over-cap announced length names the limit to the operator");
    expect(!QFile::exists(
               artifactPath(directory)),
           "an over-cap announced length leaves nothing on disk");
    recordRequests(network);
  }

  // A server that announces nothing and streams past the cap is cut off
  // mid-transfer. The injected cap keeps the fixture small; it is the same
  // code path the 256 MiB production constant drives.
  {
    constexpr qint64 kTestCap = 2LL * 1'024LL * 1'024LL;
    const QByteArray oversized(static_cast<int>(kTestCap) + 1, 'X');
    ScriptedNetworkAccessManager network;
    auto checker = primedChecker(&network, directory, pinned(kArtifactName),
                                 pinned("SHA256SUMS"), kTestCap);
    network.enqueue({.status = 200, .body = checksumList(oversized)});
    network.enqueue({.status = 200,
                     .body = oversized,
                     .declared_length = -2});
    checker->downloadUpdate();
    expect(waitUntil([&] { return !checker->downloading(); }),
           "an unannounced over-cap stream settles");
    expect(checker->statusMessage().contains(QStringLiteral("2 MiB")) &&
               !checker->downloadVerified(),
           "an unannounced over-cap stream is cut off at the limit");
    expect(!QFile::exists(
               artifactPath(directory)),
           "an over-cap stream leaves nothing at the download path");
    recordRequests(network);
  }

  // The checksum list is bounded too.
  {
    ScriptedNetworkAccessManager network;
    auto checker = primedChecker(&network, directory, pinned(kArtifactName),
                                 pinned("SHA256SUMS"));
    network.enqueue(
        {.status = 200,
         .body = QByteArray(static_cast<int>(detail::kMaximumMetadataBytes) + 1,
                            'c')});
    checker->downloadUpdate();
    expect(waitUntil([&] { return !checker->downloading(); }),
           "an over-cap checksum list settles");
    expect(network.count() == 2 &&
               checker->statusMessage().contains(QStringLiteral("64 KiB")),
           "an over-cap checksum list is discarded without a download");
    recordRequests(network);
  }

  // So is the manifest.
  {
    clearSettings();
    ScriptedNetworkAccessManager network;
    network.enqueue(
        {.status = 200,
         .body = QByteArray(static_cast<int>(detail::kMaximumMetadataBytes) + 1,
                            'm')});
    UpdateChecker checker(&network, QUrl(QString::fromLatin1(kManifestUrl)),
                          directory, detail::kMaximumArtifactBytes, nullptr);
    checker.checkForUpdates();
    expect(waitUntil([&] { return !checker.checking(); }),
           "an over-cap manifest settles");
    expect(checker.statusMessage().contains(QStringLiteral("64 KiB")) &&
               !checker.updateAvailable(),
           "an over-cap manifest is discarded without being parsed");
    recordRequests(network);
  }
}

void testStalledTransferIsReportedAndBounded(const QString& directory) {
  ScriptedNetworkAccessManager network;
  auto checker = primedChecker(&network, directory, pinned(kArtifactName),
                               pinned("SHA256SUMS"));
  network.enqueue({.status = 200, .body = checksumList(QByteArray("x"))});
  for (int attempt = 0; attempt < detail::kMaximumAttempts; ++attempt) {
    network.enqueue({.status = 0,
                     .error = QNetworkReply::OperationCanceledError,
                     .error_text = QStringLiteral("transfer timed out")});
  }
  checker->downloadUpdate();
  expect(waitUntil(
             [&] {
               return network.count() == 5 && !checker->downloading();
             },
             6'000),
         "a stalled transfer retries up to the attempt bound and then stops");
  expect(checker->statusMessage().contains(QStringLiteral("stalled")),
         "a stalled transfer is reported as a stall, not a bare cancellation");
  expect(!checker->downloading() && !checker->downloadVerified(),
         "a stalled transfer does not latch the downloading state");
  recordRequests(network);
}

void testCancelReleasesTheState(const QString& directory) {
  const QByteArray payload("second attempt payload\n");
  ScriptedNetworkAccessManager network;
  auto checker = primedChecker(&network, directory, pinned(kArtifactName),
                               pinned("SHA256SUMS"));
  network.enqueue({.status = 200, .body = checksumList(payload)});
  network.enqueue({.status = 200, .never_finishes = true});
  checker->downloadUpdate();
  expect(waitUntil([&] { return network.count() == 3; }),
         "the artifact request for the cancel case is issued");
  expect(checker->downloading(),
         "a transfer that never finishes holds the downloading state");

  checker->cancelDownload();
  expect(!checker->downloading(),
         "cancelling a stuck transfer clears the downloading state at once");
  expect(checker->statusMessage().contains(QStringLiteral("cancelled")),
         "cancelling says so to the operator");
  expect(!QFile::exists(
             artifactPath(directory)),
         "cancelling leaves nothing at the download path");

  // The late finish of the abandoned reply must not resurrect any state, and
  // the operator must be able to start over.
  network.enqueue({.status = 200, .body = checksumList(payload)});
  network.enqueue({.status = 200, .body = payload});
  checker->downloadUpdate();
  expect(waitUntil([&] { return !checker->downloading(); }),
         "a download started after a cancel completes");
  expect(checker->downloadVerified() &&
             checker->statusMessage().contains(QStringLiteral("verified")),
         "a cancelled download does not latch the state against a retry");
  expect(network.unexpectedRequests() == 0,
         "the cancel flow performs no unscripted request");
  recordRequests(network);
  QFile::remove(artifactPath(directory));
}

void testExistingVerificationBehaviourIsPreserved(const QString& directory) {
  // A transfer that ends short of the announced length stays a failed
  // download, reported with the byte counts, and never a failed checksum.
  {
    ScriptedNetworkAccessManager network;
    auto checker = primedChecker(&network, directory, pinned(kArtifactName),
                                 pinned("SHA256SUMS"));
    network.enqueue({.status = 200, .body = checksumList(QByteArray("x"))});
    for (int attempt = 0; attempt < detail::kMaximumAttempts; ++attempt) {
      network.enqueue({.status = 200,
                       .body = QByteArray(12'345, 'p'),
                       .declared_length = 53'881'388});
    }
    checker->downloadUpdate();
    expect(waitUntil(
               [&] { return network.count() == 5 && !checker->downloading(); },
               6'000),
           "a short transfer retries and then stops");
    expect(checker->statusMessage().contains(QStringLiteral("ended after")) &&
               checker->statusMessage().contains(
                   QStringLiteral("nothing was verified")),
           "a short transfer is reported as a failed download");
    expect(!checker->statusMessage().contains(QStringLiteral("hecksum")) &&
               !checker->statusMessage().contains(
                   QStringLiteral("tampered")),
           "a short transfer never accuses the release of being corrupt");
    expect(!QFile::exists(
               artifactPath(directory)),
           "a short transfer leaves nothing at the download path");
    recordRequests(network);
  }

  // A complete transfer whose bytes are wrong is still a checksum failure,
  // and the unverified bytes never appear at the operator-visible path.
  {
    const QByteArray published("the release that was published\n");
    const QByteArray served("something else entirely\n");
    ScriptedNetworkAccessManager network;
    auto checker = primedChecker(&network, directory, pinned(kArtifactName),
                                 pinned("SHA256SUMS"));
    network.enqueue({.status = 200, .body = checksumList(published)});
    network.enqueue({.status = 200, .body = served});
    checker->downloadUpdate();
    expect(waitUntil([&] { return !checker->downloading(); }),
           "a checksum mismatch settles");
    expect(checker->statusMessage().contains(QStringLiteral("Checksum "
                                                            "mismatch")),
           "a complete but wrong payload is reported as a checksum mismatch");
    expect(!checker->downloadVerified() &&
               checker->downloadedFilePath().isEmpty(),
           "a checksum mismatch exposes no downloaded file");
    expect(!QFile::exists(
               artifactPath(directory)),
           "a checksum mismatch never writes the unverified bytes into place");
    recordRequests(network);
  }

  // A payload with no published checksum is refused rather than saved.
  {
    const QByteArray payload("no line for this file\n");
    ScriptedNetworkAccessManager network;
    auto checker = primedChecker(&network, directory, pinned(kArtifactName),
                                 pinned("SHA256SUMS"));
    network.enqueue({.status = 200,
                     .body = checksumList(payload,
                                          QStringLiteral("other-asset.bin"))});
    network.enqueue({.status = 200, .body = payload});
    checker->downloadUpdate();
    expect(waitUntil([&] { return !checker->downloading(); }),
           "a missing checksum line settles");
    expect(checker->statusMessage().contains(
               QStringLiteral("Could not find a published checksum")),
           "a missing checksum line is reported as such");
    expect(!QFile::exists(
               artifactPath(directory)),
           "a missing checksum line leaves nothing at the download path");
    recordRequests(network);
  }

  // A transient server failure still retries with bounded backoff.
  {
    const QByteArray payload("published after a propagation delay\n");
    ScriptedNetworkAccessManager network;
    auto checker = primedChecker(&network, directory, pinned(kArtifactName),
                                 pinned("SHA256SUMS"));
    network.enqueue({.status = 200, .body = checksumList(payload)});
    network.enqueue({.status = 404,
                     .error = QNetworkReply::ContentNotFoundError});
    network.enqueue({.status = 200, .body = payload});
    checker->downloadUpdate();
    expect(waitUntil([&] { return !checker->downloading(); }, 4'000),
           "a transient artifact failure settles");
    expect(checker->downloadVerified() && network.count() == 4,
           "a transient artifact failure is retried and then verifies");
    recordRequests(network);
    QFile::remove(artifactPath(directory));
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("CW Buddy Tests"));
  QCoreApplication::setOrganizationDomain(QStringLiteral("invalid.test"));
  QCoreApplication::setApplicationName(QStringLiteral("Update Checker Test"));
  QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
  QStandardPaths::setTestModeEnabled(true);

  using cwassistant::desktop::update_detail::isTransientFailure;
  using cwassistant::desktop::update_detail::retryDelayMs;
  using cwassistant::desktop::update_detail::updateActionVisibility;

  if (!isTransientFailure(QNetworkReply::ContentNotFoundError, 404) ||
      !isTransientFailure(QNetworkReply::InternalServerError, 500) ||
      !isTransientFailure(QNetworkReply::TimeoutError, 0) ||
      isTransientFailure(QNetworkReply::AuthenticationRequiredError, 401) ||
      isTransientFailure(QNetworkReply::ProtocolInvalidOperationError, 400)) {
    return 1;
  }
  // A deliberate cancellation is not a transient failure, so an operator's
  // cancel can never be retried back into existence. The stall that Qt's
  // transfer timeout reports under the same error code is recognised at the
  // call site, where the reason for the abort is known.
  if (isTransientFailure(QNetworkReply::OperationCanceledError, 0)) {
    return 7;
  }
  // A transfer that ends short of what the server announced is a failed
  // download, not a failed verification. Reporting it as a checksum failure
  // accuses an intact release of being corrupt, which is what an operator
  // acted on before this: a gateway timeout produced "checksum check failed"
  // and the release it named verified byte for byte when checked by hand.
  using cwassistant::desktop::update_detail::isIncompleteTransfer;
  if (!isIncompleteTransfer(53'881'388, 12'345) ||
      !isIncompleteTransfer(2, 1) ||
      isIncompleteTransfer(53'881'388, 53'881'388) ||
      isIncompleteTransfer(53'881'388, 53'881'389)) {
    return 4;
  }
  // A server that declares no length says nothing about completeness, and a
  // guess either way would be wrong: claiming incomplete would refuse every
  // chunked transfer, and there is nothing to compare against.
  if (isIncompleteTransfer(0, 0) || isIncompleteTransfer(0, 53'881'388) ||
      isIncompleteTransfer(-1, 0) || isIncompleteTransfer(-1, 53'881'388)) {
    return 5;
  }
  // An empty body against a declared length is the shape a gateway error page
  // discarded by the network stack leaves behind.
  if (!isIncompleteTransfer(53'881'388, 0)) {
    return 6;
  }

  if (retryDelayMs(1) != 400 || retryDelayMs(2) != 1'200 ||
      retryDelayMs(3) != 2'500) {
    return 2;
  }

  const auto hidden = updateActionVisibility(false, true, false);
  const auto unsupported = updateActionVisibility(true, false, false);
  const auto downloadable = updateActionVisibility(true, true, false);
  const auto verified = updateActionVisibility(true, true, true);
  const auto retained_verified = updateActionVisibility(false, false, true);
  if (hidden.row || hidden.download || hidden.verified_download ||
      unsupported.row || unsupported.download ||
      unsupported.verified_download || !downloadable.row ||
      !downloadable.download || downloadable.verified_download ||
      !verified.row || verified.download || !verified.verified_download ||
      !retained_verified.row || retained_verified.download ||
      !retained_verified.verified_download) {
    return 3;
  }

  testTrustRule();
  testRedirectRule();

  QTemporaryDir directory;
  if (!directory.isValid()) return 90;
  const QString download_directory =
      QDir(directory.path()).filePath(QStringLiteral("downloads"));
  if (!QDir().mkpath(download_directory)) return 91;

  // The remaining cases need this platform to have a published artifact; the
  // manifest fixture names all five keys, so only a platform the shipping
  // build does not package can miss.
  {
    UpdateChecker probe;
    if (!probe.platformSupported()) {
      std::cout << "Update checker tests passed (no packaged artifact for "
                   "this platform)\n";
      return failures == 0 ? 0 : 8;
    }
  }

  testVerifiedDownloadAndRequestHardening(download_directory);
  testUntrustedManifestUrlsNeverReachTheNetwork(download_directory);
  testRedirectsAreCheckedOnEveryHop(download_directory);
  testSizeCaps(download_directory);
  testStalledTransferIsReportedAndBounded(download_directory);
  testCancelReleasesTheState(download_directory);
  testExistingVerificationBehaviourIsPreserved(download_directory);

  // Nothing in this file may ever have issued a request to a host that only
  // appeared as bait inside a manifest or a redirect.
  for (const QUrl& url : all_requested_urls) {
    const QString host = url.host().toLower();
    if (host != QStringLiteral("github.com") &&
        host != QStringLiteral("objects.githubusercontent.com")) {
      ++failures;
      std::cerr << "FAIL: a request was issued to an untrusted host: "
                << host.toStdString() << '\n';
    }
  }

  clearSettings();
  if (failures == 0) {
    std::cout << "Update checker tests passed\n";
    return 0;
  }
  std::cerr << failures << " update checker test(s) failed\n";
  return 8;
}
