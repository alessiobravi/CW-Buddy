#include <QCoreApplication>
#include <QNetworkReply>

#include "updates/update_checker.hpp"

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
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
  return 0;
}
