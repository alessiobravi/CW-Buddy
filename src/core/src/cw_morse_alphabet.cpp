#include "cwassistant/core/cw_morse_alphabet.hpp"

#include <cctype>

#include "cwassistant/core/cw_builtin_morse_alphabet.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace cwassistant::core {
namespace {

std::string_view trim(std::string_view text) {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.remove_suffix(1);
  }
  return text;
}

// The fallback chain the shared alphabet recovers through, performed on an
// alphabet the caller has already found empty.
//
// noexcept because one of its two callers is declared noexcept and the other
// is reached from noexcept decode paths: a dictionary that cannot be read has
// to stay a failure to load rather than become a terminate.
void loadSharedMorseAlphabet(CwMorseAlphabet& alphabet) noexcept {
  // Decoding nothing at all is a far worse failure than reading an
  // environment variable, so a caller that never loaded the alphabet recovers
  // here rather than returning an unknown symbol for every character.
  try {
    const char* directory = std::getenv("CWA_DICTIONARY_DIR");
    if (directory != nullptr) {
      std::ifstream input(std::string(directory) + "/morse-alphabet.txt",
                          std::ios::binary);
      if (input) {
        std::ostringstream buffer;
        buffer << input.rdbuf();
        static_cast<void>(alphabet.importText(buffer.str()));
      }
    }
  } catch (...) {
    // A read that threw part way through leaves a partial alphabet behind
    // with the loaded origin recorded against it. Discard it so the
    // compiled-in copy below takes over whole and names itself as the origin.
    alphabet.clear();
  }
  if (!alphabet.empty()) return;
  // Last resort. An operator's file that is missing, empty, or unreadable must
  // not leave the application tracking signals and decoding nothing, which is
  // what it did: the alphabet was made data with no fallback, and a packaged
  // build that failed to load it went silently deaf with no diagnostic. The
  // text below is generated from the same shipped file at build time, so this
  // is not a second table to keep in step -- it is the same one, compiled in.
  try {
    static_cast<void>(alphabet.importText(kCwBuiltinMorseAlphabet,
                                          CwMorseAlphabetSource::kBuiltin));
  } catch (...) {
    alphabet.clear();
  }
}

// Loads the shared alphabet if nothing has loaded one yet.
//
// The first use is behind C++ thread-safe static initialization, because it
// is the only one that can happen on several threads at once: a host that
// never loaded the alphabet eagerly has every decode thread arriving at an
// empty alphabet together, and the plain "if empty, import" this replaced had
// them all building the same unordered_map at the same time.
void ensureSharedMorseAlphabetLoaded() noexcept {
  auto& alphabet = cwMutableSharedMorseAlphabet();
  static const bool attempted = [&alphabet]() noexcept {
    if (alphabet.empty()) loadSharedMorseAlphabet(alphabet);
    return true;
  }();
  static_cast<void>(attempted);
  // A later clear() still recovers. The application clears the alphabet
  // before importing the operator's file, and a test does the same to
  // exercise the fallback, so a once-and-never-again guard would hand both an
  // empty alphabet and a silently deaf decoder -- exactly the fault the
  // fallback exists to prevent. Unlike first use, this path is not concurrent
  // by construction: it can only follow a clear() on the shared alphabet,
  // which is already an exclusive operation on it.
  if (alphabet.empty()) loadSharedMorseAlphabet(alphabet);
}

}  // namespace

CwMorseAlphabetImportResult CwMorseAlphabet::importText(
    const std::string_view text, const CwMorseAlphabetSource source) {
  CwMorseAlphabetImportResult result;
  result.accepted = true;
  std::size_t offset = 0;
  while (offset <= text.size()) {
    const auto newline = text.find('\n', offset);
    const auto end = newline == std::string_view::npos ? text.size() : newline;
    const auto line = trim(text.substr(offset, end - offset));
    if (!line.empty() && line.front() != '#') {
      const auto split = line.find_first_of(" \t");
      const auto code = split == std::string_view::npos
          ? std::string_view{} : trim(line.substr(0, split));
      const auto symbol = split == std::string_view::npos
          ? std::string_view{} : trim(line.substr(split));
      const bool well_formed = !code.empty() && !symbol.empty() &&
          code.find_first_not_of(".-") == std::string_view::npos;
      if (!well_formed) {
        ++result.ignored_lines;
      } else if (symbols_.emplace(std::string(code),
                                  std::string(symbol)).second) {
        ++result.inserted_symbols;
      } else {
        ++result.duplicate_codes;
      }
    }
    if (newline == std::string_view::npos) break;
    offset = newline + 1U;
  }
  // The last import that actually contributed a symbol defines the origin of
  // the contents. Nothing here merges two origins in practice: a host clears
  // the alphabet before loading a file, and the compiled-in copy is only ever
  // imported into an alphabet that is still empty.
  if (result.inserted_symbols > 0) source_ = source;
  return result;
}

std::string_view CwMorseAlphabet::symbolFor(
    const std::string_view elements) const {
  const auto found = symbols_.find(std::string(elements));
  return found == symbols_.end() ? std::string_view{} : found->second;
}

std::size_t CwMorseAlphabet::size() const noexcept { return symbols_.size(); }
bool CwMorseAlphabet::empty() const noexcept { return symbols_.empty(); }

CwMorseAlphabetSource CwMorseAlphabet::source() const noexcept {
  return source_;
}

void CwMorseAlphabet::clear() noexcept {
  symbols_.clear();
  // An emptied alphabet has no origin. Leaving the previous one in place would
  // let a host that clears and then fails to load report the origin of symbols
  // that are no longer there.
  source_ = CwMorseAlphabetSource::kNone;
}

CwMorseAlphabet& cwMutableSharedMorseAlphabet() noexcept {
  static CwMorseAlphabet alphabet;
  return alphabet;
}

const CwMorseAlphabet& cwSharedMorseAlphabet() {
  ensureSharedMorseAlphabetLoaded();
  return cwMutableSharedMorseAlphabet();
}

bool cwMorseAlphabetLoadedFromBuiltin() noexcept {
  // Asking where the alphabet came from has to answer for the alphabet the
  // next decode will actually use, so it loads one the same way that decode
  // would. Reading the recorded origin without doing so answered "not the
  // compiled-in copy" while nothing was loaded at all -- which is the answer
  // a perfectly healthy file load gives, and therefore precisely the
  // backwards reading this flag exists to prevent.
  ensureSharedMorseAlphabetLoaded();
  return cwMutableSharedMorseAlphabet().source() ==
      CwMorseAlphabetSource::kBuiltin;
}

}  // namespace cwassistant::core
