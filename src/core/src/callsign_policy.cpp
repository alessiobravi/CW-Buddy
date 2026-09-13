#include "cwassistant/core/callsign_policy.hpp"
#include "cwassistant/core/cw_callsign_prefixes.hpp"
#include "cwassistant/core/cw_vocabulary.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cwassistant::core {
namespace {

bool isPlausibleDecodedCallsign(const std::string_view normalized) {
  const auto slash = normalized.find('/');
  if (slash != std::string_view::npos &&
      normalized.find('/', slash + 1) != std::string_view::npos) return false;
  const auto plausible_base = [](const std::string_view base) {
    if (base.size() < 3 || base.size() > 12) return false;
    const auto first_digit = base.find_first_of("0123456789");
    const auto last_digit = base.find_last_of("0123456789");
    if (first_digit == std::string_view::npos ||
        last_digit + 1 >= base.size() || base.size() - last_digit - 1 > 4) {
      return false;
    }
    const bool standard_prefix = first_digit > 0 && std::all_of(
        base.begin(), base.begin() + static_cast<std::ptrdiff_t>(first_digit),
        [](const unsigned char character) {
          return std::isalpha(character) != 0;
        });
    const auto second_digit = first_digit == 0
        ? base.find_first_of("0123456789", 1) : std::string_view::npos;
    const bool numeric_leading_prefix = second_digit != std::string_view::npos &&
        second_digit > 1 && std::all_of(
            base.begin() + 1,
            base.begin() + static_cast<std::ptrdiff_t>(second_digit),
            [](const unsigned char character) {
              return std::isalpha(character) != 0;
            }) && std::all_of(
            base.begin() + static_cast<std::ptrdiff_t>(second_digit),
            base.begin() + static_cast<std::ptrdiff_t>(last_digit + 1),
            [](const unsigned char character) {
              return std::isdigit(character) != 0;
            });
    if ((!standard_prefix && !numeric_leading_prefix) || !std::all_of(
            base.begin() + static_cast<std::ptrdiff_t>(last_digit + 1),
            base.end(), [](const unsigned char character) {
              return std::isalpha(character) != 0;
            })) {
      return false;
    }
    // A contiguous multi-digit block is valid for special-event calls. A
    // letter-leading token with separated digit runs is far more likely to be
    // merged report/noise (for example EA7G2NX) than one decoded callsign.
    if (standard_prefix) {
      return std::all_of(
          base.begin() + static_cast<std::ptrdiff_t>(first_digit),
          base.begin() + static_cast<std::ptrdiff_t>(last_digit + 1),
          [](const unsigned char character) {
            return std::isdigit(character) != 0;
          });
    }
    return true;
  };
  if (slash == std::string_view::npos) return plausible_base(normalized);
  const std::string_view left = normalized.substr(0, slash);
  const std::string_view right = normalized.substr(slash + 1);
  const auto plausible_modifier = [](const std::string_view value) {
    return !value.empty() && value.size() <= 4 &&
        std::all_of(value.begin(), value.end(),
                    [](const unsigned char character) {
                      return std::isalnum(character) != 0;
                    });
  };
  return (plausible_base(left) && plausible_modifier(right)) ||
         (plausible_modifier(left) && plausible_base(right));
}

// The least evidence a decoded token may carry and still name a stream.
//
// Four, not three. Three is exactly what a callsign-shaped token earns for
// standing third after a CQ, which is the weakest positional rule there is --
// a guess about where a call sits in a transmission, saying nothing about
// whether its characters were copied right. On the decoder surface benchmark
// admitting that alone costs a wrong callsign and gains none: 26 correct and 5
// wrong at four, against 26 and 6 at three.
constexpr int kMinimumLabelScore = 4;

// What the two decode paths reading the same callsign is worth.
//
// The literal and the lattice-refined texts are independent readings of one
// transmission -- that independence is the whole reason the refined path
// exists -- so a complete callsign standing whole in both is evidence about
// the signal, not about where a call was expected to sit. Six places it above
// a call standing next to a CQ (5), which is the strongest purely positional
// rule, and below an explicit DE handover (8), which is the station saying
// whose transmission this is. It clears the floor on its own, because a call
// repeated into two agreeing decodes needs no adjacent word to vouch for it.
constexpr int kCrossPathAgreementScore = 6;

// The completed words of one decoded text, in the order every rule reads them.
//
// Completed means before the last word gap: the token still being sent is not
// yet a reading of anything, and promoting it would name a stream from half a
// callsign.
std::vector<std::string> decodedWords(const std::string_view stable_text) {
  const auto completed_end = stable_text.find_last_of(" \t\r\n");
  if (completed_end == std::string_view::npos) return {};
  std::vector<std::string> words;
  std::string token;
  for (const unsigned char character :
       stable_text.substr(0, completed_end + 1)) {
    if (std::isalnum(character) != 0 || character == '/') {
      token.push_back(static_cast<char>(std::toupper(character)));
    } else if (!token.empty()) {
      // A missing word gap glues a leading prosign onto the callsign that
      // follows it, and the glued token then collects the very context credit
      // the callsign earned: an observed CQ CQ DE SV7BIO decoded as
      // "CQ CQ DESV7BIO SV7BIO" labelled the stream DESV7BIO, even though the
      // real callsign stood alone twice in the same text. Split the pair only
      // when what follows the prosign is itself a plausible callsign, so a
      // genuine DE-prefixed German call is untouched -- stripping DE from
      // DE1ABC leaves 1ABC, which is not a callsign, and the token stands.
      // The same set the context rescorer splits on, from
      // dictionaries/cw-word-gap-prefixes.txt: two copies of this list is how
      // the two paths come to disagree about the same text.
      bool split = false;
      for (const std::string_view prefix : cwSharedVocabulary().wordGapPrefixes()) {
        if (token.size() <= prefix.size() ||
            token.compare(0, prefix.size(), prefix) != 0) {
          continue;
        }
        const std::string remainder = token.substr(prefix.size());
        const auto normalized = CallsignPolicy::normalize(remainder);
        if (!normalized || !isPlausibleDecodedCallsign(*normalized)) continue;
        words.emplace_back(prefix);
        words.push_back(remainder);
        split = true;
        break;
      }
      if (!split) words.push_back(token);
      token.clear();
    }
  }
  return words;
}

// The complete callsigns both decode paths produced, whole.
//
// Whole tokens on both sides, never a substring: EH3ST found inside XEH3STY is
// a different decode, and counting it as agreement would let one path's noise
// confirm the other path's reading. The token has to be callsign-shaped, and
// its prefix has to be one the ITU allocated, because an agreed token that the
// publication gate will refuse anyway must not be allowed to outrank a
// candidate that would have survived it -- that trades a name for no name.
//
// An empty text on either side agrees with nothing, which is what keeps the
// signal silent when the literal path's timing gate withheld its text: one
// reading is not two.
std::unordered_set<std::string> agreedCallsigns(
    const std::vector<std::string>& primary_words,
    const std::vector<std::string>& refined_words) {
  std::unordered_set<std::string> primary_callsigns;
  for (const auto& word : primary_words) {
    const auto normalized = CallsignPolicy::normalize(word);
    if (normalized && isPlausibleDecodedCallsign(*normalized) &&
        cwSharedCallsignPrefixes().isAllocatedPrefix(*normalized)) {
      primary_callsigns.insert(*normalized);
    }
  }
  if (primary_callsigns.empty()) return {};
  std::unordered_set<std::string> agreed;
  for (const auto& word : refined_words) {
    const auto normalized = CallsignPolicy::normalize(word);
    if (normalized && primary_callsigns.contains(*normalized)) {
      agreed.insert(*normalized);
    }
  }
  return agreed;
}

std::optional<std::string> bestCompleteInWords(
    const std::vector<std::string>& words, const CwOperatorRole role,
    const std::string_view own_callsign,
    const std::unordered_set<std::string>& agreed) {
  struct CandidateEvidence {
    int score{0};
    std::size_t first_index{0};
    std::size_t last_index{0};
    int occurrences{0};
    bool agreed{false};
  };
  std::unordered_map<std::string, CandidateEvidence> evidence;
  for (std::size_t index = 0; index < words.size(); ++index) {
    const auto normalized = CallsignPolicy::normalize(words[index]);
    if (!normalized || !isPlausibleDecodedCallsign(*normalized)) continue;
    CandidateEvidence& candidate = evidence[*normalized];
    ++candidate.occurrences;
    if (candidate.occurrences == 1) candidate.first_index = index;
    candidate.last_index = index;
    // A stream label needs more than callsign-shaped spelling. Contest and
    // ordinary CW exchanges provide useful role evidence: a station normally
    // identifies itself after DE, CQ/TEST, or TU, and a split runner commonly
    // places UP immediately after its call. Exact repetition is weaker but
    // still useful for a caller sending only its own call. These weights rank
    // decoded alternatives; they never create or correct decoded characters.
    if (index > 0 && words[index - 1] == "DE") candidate.score += 8;
    // TU is genuinely ambiguous: it precedes a runner identifying itself and
    // equally the station it has just worked. Where the operator's role says
    // which of the two the monitored stream is, the unambiguous contexts are
    // allowed to outrank it rather than tie with it.
    if (index > 0 && words[index - 1] == "TU") {
      candidate.score += role == CwOperatorRole::Monitor ? 6 : 4;
    }
    if (index > 0 && words[index - 1] == "CQ") candidate.score += 5;
    if (index > 1 && words[index - 2] == "CQ") candidate.score += 4;
    if (index > 2 && words[index - 3] == "CQ") candidate.score += 3;
    if (index + 1 < words.size() && words[index + 1] == "UP")
      candidate.score += 5;
    // Ordinary hand-sent QSOs often close an identification with PSE K, K,
    // KN, AR, or SK rather than repeating CQ/DE. These are role/boundary
    // clues only: the token must already be a complete acoustically decoded
    // callsign before any of these weights apply.
    if (index + 2 < words.size() && words[index + 1] == "PSE" &&
        words[index + 2] == "K") {
      candidate.score += 6;
    } else if (index + 1 < words.size() &&
               (words[index + 1] == "K" || words[index + 1] == "KN" ||
                words[index + 1] == "AR" || words[index + 1] == "SK")) {
      candidate.score += 4;
    }
    if (candidate.occurrences > 1) candidate.score += 4;
    // Searching and pouncing, the stream being listened to is a runner, so the
    // call introduced as the sender's own outranks one merely mentioned.
    // Running, it is a station answering, which sends its call bare and
    // repeats it rather than introducing it.
    if (role == CwOperatorRole::SearchAndPounce) {
      const bool runner_context =
          (index > 0 && (words[index - 1] == "CQ" || words[index - 1] == "DE")) ||
          (index > 1 && words[index - 2] == "CQ") ||
          (index + 1 < words.size() && words[index + 1] == "UP");
      if (runner_context) candidate.score += 3;
    } else if (role == CwOperatorRole::Runner) {
      if (candidate.occurrences > 1) candidate.score += 3;
      // A caller answering does not call CQ; a CQ in the monitored stream
      // belongs to somebody else's transmission bleeding into the same slice.
      if (index > 0 && words[index - 1] == "CQ") candidate.score -= 3;
    }
  }

  // Cross-path agreement is a property of the candidate, not of a position, so
  // it is credited once however often the call was copied. It does not reorder
  // anything by itself: positional evidence still decides between two calls the
  // two paths both read, which is what keeps the sender of a CALL1 DE CALL2
  // handover ahead of the station it addressed when both were copied cleanly.
  for (const auto& candidate : agreed) {
    if (const auto found = evidence.find(candidate); found != evidence.end()) {
      found->second.score += kCrossPathAgreementScore;
      found->second.agreed = true;
    }
  }

  // Whatever the role, a call the operator's own station sends cannot be the
  // label for another station's stream. Own-call detection elsewhere alerts the
  // operator that they were called; that is a different question from whose
  // transmission this is.
  if (!own_callsign.empty()) {
    const auto own = CallsignPolicy::normalize(own_callsign);
    if (own) evidence.erase(*own);
  }

  // Which candidate names the stream, in three steps.
  //
  // Evidence first. Where the totals tie, the call both decode paths read wins
  // over one only a positional rule vouches for: the same number arrived at
  // from a reading of the signal rather than from a guess about where a call
  // sits. Between two calls the paths both read, the earliest wins, and that is
  // the one rule here that reverses the general preference for the newest
  // reading. The decoded text is append-only, so a token's place in it is when
  // it was copied: an agreement standing since early in the transmission has
  // been holding for the whole stretch since, while one that has just appeared
  // has held for an instant. Positional evidence carries no such history -- a
  // CQ beside a call says the same thing wherever it falls, and there the
  // latest reading is the one that says who is transmitting now -- so ties
  // between candidates neither path corroborated still go to the last read.
  const auto outranks = [](const CandidateEvidence& left,
                           const CandidateEvidence& right) {
    if (left.score != right.score) return left.score > right.score;
    if (left.agreed != right.agreed) return left.agreed;
    if (left.agreed) return left.first_index < right.first_index;
    return left.last_index > right.last_index;
  };
  std::optional<std::string> result;
  const CandidateEvidence* best = nullptr;
  for (const auto& [candidate, value] : evidence) {
    if (value.score < kMinimumLabelScore) continue;
    if (best != nullptr && !outranks(value, *best)) continue;
    result = candidate;
    best = &value;
  }
  return result;
}

}  // namespace

std::optional<std::string> CallsignPolicy::normalize(
    const std::string_view callsign) {
  const auto first = callsign.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return std::nullopt;
  }
  const auto last = callsign.find_last_not_of(" \t\r\n");
  const auto trimmed = callsign.substr(first, last - first + 1);
  if (trimmed.size() < 3 || trimmed.size() > 16) {
    return std::nullopt;
  }

  std::string normalized;
  normalized.reserve(trimmed.size());
  bool has_letter = false;
  bool has_digit = false;
  bool previous_was_slash = false;
  for (const unsigned char character : trimmed) {
    if (std::isalpha(character) != 0) {
      normalized.push_back(static_cast<char>(std::toupper(character)));
      has_letter = true;
      previous_was_slash = false;
    } else if (std::isdigit(character) != 0) {
      normalized.push_back(static_cast<char>(character));
      has_digit = true;
      previous_was_slash = false;
    } else if (character == '/' && !normalized.empty() && !previous_was_slash) {
      normalized.push_back('/');
      previous_was_slash = true;
    } else {
      return std::nullopt;
    }
  }

  if (!has_letter || !has_digit || normalized.back() == '/') {
    return std::nullopt;
  }
  return normalized;
}

std::optional<std::string> CallsignPolicy::latest_in_text(
    const std::string_view decoded_text) {
  std::string token;
  std::optional<std::string> result;
  const auto consider = [&result](const std::string& candidate) {
    if (const auto normalized = normalize(candidate);
        normalized && isPlausibleDecodedCallsign(*normalized)) {
      result = *normalized;
    }
  };
  for (const unsigned char character : decoded_text) {
    if (std::isalnum(character) != 0 || character == '/') {
      token.push_back(static_cast<char>(character));
    } else if (!token.empty()) {
      consider(token);
      token.clear();
    }
  }
  if (!token.empty()) consider(token);
  return result;
}

std::optional<std::string> CallsignPolicy::latest_complete_in_text(
    const std::string_view stable_text) {
  const auto completed_end = stable_text.find_last_of(" \t\r\n");
  if (completed_end == std::string_view::npos) return std::nullopt;
  return latest_in_text(stable_text.substr(0, completed_end + 1));
}

std::vector<std::string> CallsignPolicy::qso_participants_in_text(
    const std::string_view stable_text) {
  const auto completed_end = stable_text.find_last_of(" \t\r\n");
  if (completed_end == std::string_view::npos) return {};
  std::vector<std::string> words;
  std::string token;
  for (const unsigned char character :
       stable_text.substr(0, completed_end + 1)) {
    if (std::isalnum(character) != 0 || character == '/') {
      token.push_back(static_cast<char>(std::toupper(character)));
    } else if (!token.empty()) {
      words.push_back(std::move(token));
      token.clear();
    }
  }
  std::vector<std::string> latest_pair;
  for (std::size_t index = 1; index + 1 < words.size(); ++index) {
    if (words[index] != "DE") continue;
    const auto left = normalize(words[index - 1]);
    const auto right = normalize(words[index + 1]);
    if (!left || !right || *left == *right ||
        !isPlausibleDecodedCallsign(*left) ||
        !isPlausibleDecodedCallsign(*right)) {
      continue;
    }
    latest_pair = {*left, *right};
  }
  return latest_pair;
}

std::optional<std::string> CallsignPolicy::strong_sender_in_text(
    const std::string_view stable_text) {
  const auto completed_end = stable_text.find_last_of(" \t\r\n");
  if (completed_end == std::string_view::npos) return std::nullopt;
  std::vector<std::string> words;
  std::string token;
  for (const unsigned char character :
       stable_text.substr(0, completed_end + 1)) {
    if (std::isalnum(character) != 0 || character == '/') {
      token.push_back(static_cast<char>(std::toupper(character)));
    } else if (!token.empty()) {
      words.push_back(std::move(token));
      token.clear();
    }
  }

  std::optional<std::string> sender;
  for (std::size_t index = 1; index + 1 < words.size(); ++index) {
    if (words[index] != "DE") continue;
    const auto candidate = normalize(words[index + 1]);
    if (!candidate || !isPlausibleDecodedCallsign(*candidate)) continue;

    // A decoded callsign immediately before DE is the clearest two-party
    // handover. CQ/QRZ before DE is also explicit self-identification, but an
    // isolated "DE CALL" without either side is intentionally not enough:
    // weak text can manufacture DE surprisingly often.
    const auto addressed = normalize(words[index - 1]);
    const bool addressed_call = addressed &&
        isPlausibleDecodedCallsign(*addressed) && *addressed != *candidate;
    const bool calling_context = words[index - 1] == "CQ" ||
                                 words[index - 1] == "QRZ" ||
                                 (index > 1 && words[index - 2] == "CQ");
    if (addressed_call || calling_context) sender = *candidate;
  }
  return sender;
}

std::optional<std::string> CallsignPolicy::best_complete_in_text(
    const std::string_view stable_text) {
  return best_complete_in_text(stable_text, CwOperatorRole::Monitor,
                               std::string_view{});
}

std::optional<std::string> CallsignPolicy::best_complete_in_text(
    const std::string_view stable_text, const CwOperatorRole role,
    const std::string_view own_callsign) {
  // One text on its own has no second reading to agree with it.
  return bestCompleteInWords(decodedWords(stable_text), role, own_callsign,
                             {});
}

std::optional<std::string> CallsignPolicy::best_complete_in_parallel_texts(
    const std::string_view primary_text,
    const std::string_view refined_text,
    const CwOperatorRole role,
    const std::string_view own_callsign) {
  const auto primary_words = decodedWords(primary_text);
  const auto refined_words = decodedWords(refined_text);
  // Agreement is decided once, over both word lists, and then offered to every
  // scoring pass below. A call the two paths both produced is the same evidence
  // whichever text is being read, so scoring one text without it would let the
  // literal path settle on a call its own refinement contradicts.
  const auto agreed = agreedCallsigns(primary_words, refined_words);
  const auto primary =
      bestCompleteInWords(primary_words, role, own_callsign, agreed);
  const auto refined =
      bestCompleteInWords(refined_words, role, own_callsign, agreed);
  if (!refined) return primary;
  if (primary) {
    if (*primary == *refined) return primary;
    std::string combined;
    combined.reserve(primary_text.size() + refined_text.size() + 1U);
    combined.append(primary_text);
    combined.push_back(' ');
    combined.append(refined_text);
    return bestCompleteInWords(decodedWords(combined), role, own_callsign,
                               agreed);
  }

  std::size_t exact_occurrences = 0;
  std::string token;
  const auto count_token = [&] {
    const auto normalized = normalize(token);
    if (normalized && *normalized == *refined) ++exact_occurrences;
    token.clear();
  };
  for (const unsigned char character : refined_text) {
    if (std::isalnum(character) != 0 || character == '/') {
      token.push_back(static_cast<char>(character));
    } else if (!token.empty()) {
      count_token();
    }
  }
  if (!token.empty()) count_token();
  // Reaching here means only the refined path read this call: had the literal
  // text carried it too, agreement would have carried it out of the branch
  // above. One path on its own has to say it twice, or hand the transmission
  // over explicitly, before it names a stream -- standing beside a CQ is not
  // enough when the reading that could have contradicted it is missing. This
  // is where the whole failure ends, on the operator's pileup: the literal
  // path's timing gate closed for the last second and a half of the capture,
  // and a single refined E5Q next to a CQ took the stream away from a name the
  // two paths had agreed on for the preceding minute.
  if (exact_occurrences >= 2U) return refined;
  const auto sender = strong_sender_in_text(refined_text);
  return sender && *sender == *refined ? refined : std::nullopt;
}

bool CallsignPolicy::add_ignored(const std::string_view callsign) {
  const auto normalized = normalize(callsign);
  return normalized.has_value() && ignored_.insert(*normalized).second;
}

bool CallsignPolicy::remove_ignored(const std::string_view callsign) {
  const auto normalized = normalize(callsign);
  return normalized.has_value() && ignored_.erase(*normalized) == 1;
}

bool CallsignPolicy::is_ignored(const std::string_view callsign) const {
  const auto normalized = normalize(callsign);
  return normalized.has_value() && ignored_.contains(*normalized);
}

std::vector<std::string> CallsignPolicy::ignored_callsigns() const {
  std::vector<std::string> result(ignored_.begin(), ignored_.end());
  std::ranges::sort(result);
  return result;
}

}  // namespace cwassistant::core
