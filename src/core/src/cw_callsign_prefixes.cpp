#include "cwassistant/core/cw_callsign_prefixes.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "cwassistant/core/cw_builtin_callsign_prefixes.hpp"

namespace cwassistant::core {
namespace {

using Bound = std::array<char, 3>;

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

char upper(const char value) {
  return static_cast<char>(
      std::toupper(static_cast<unsigned char>(value)));
}

// Compares two three-character bounds as unsigned bytes, so the ordering is
// the same whether or not char is signed on this platform. The whole scheme
// rests on a digit sorting below every letter, which is only reliably true of
// the unsigned comparison.
int compareBounds(const Bound& left, const Bound& right) {
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto first = static_cast<unsigned char>(left[index]);
    const auto second = static_cast<unsigned char>(right[index]);
    if (first != second) return first < second ? -1 : 1;
  }
  return 0;
}

// The callsign's first `length` characters, upper-cased and padded to three
// with 'A'.
//
// The padding is what makes the scheme work on real callsigns. A digit sorts
// below every letter, so W0ZA taken as W0Z lies outside WAA-WZZ: comparing
// three characters and stopping there would refuse most of the United States,
// and every G4, K1 and I2 call with it. Padding the shorter prefixes with the
// lowest letter puts them at the foot of their own block instead.
Bound paddedPrefix(const std::string_view callsign, const std::size_t length) {
  Bound bound{'A', 'A', 'A'};
  const auto taken = std::min(length, callsign.size());
  for (std::size_t index = 0; index < taken; ++index) {
    bound[index] = upper(callsign[index]);
  }
  return bound;
}

}  // namespace

CwCallsignPrefixImportResult CwCallsignPrefixTable::importText(
    const std::string_view text, const CwCallsignPrefixSource source) {
  CwCallsignPrefixImportResult result;
  result.accepted = true;
  const auto before = blocks_.size();
  std::size_t parsed = 0;
  std::size_t offset = 0;
  while (offset <= text.size()) {
    const auto newline = text.find('\n', offset);
    const auto end = newline == std::string_view::npos ? text.size() : newline;
    const auto line = trim(text.substr(offset, end - offset));
    if (!line.empty() && line.front() != '#') {
      const auto start_end = line.find_first_of(" \t");
      const auto start_text = line.substr(0, start_end);
      const auto remainder = start_end == std::string_view::npos
          ? std::string_view{} : trim(line.substr(start_end));
      const auto end_end = remainder.find_first_of(" \t");
      const auto end_text = remainder.substr(0, end_end);
      // A line that names no country is still a usable block. The names are
      // there for a human reading the file and decide nothing, so refusing the
      // block over a missing one would throw away every station it covers --
      // the one direction this table must not fail in.
      const auto country = end_end == std::string_view::npos
          ? std::string_view{} : trim(remainder.substr(end_end));
      Block block;
      block.start = paddedPrefix(start_text, 3);
      block.end = paddedPrefix(end_text, 3);
      const bool well_formed = start_text.size() == 3 &&
          end_text.size() == 3 && compareBounds(block.start, block.end) <= 0;
      if (!well_formed) {
        ++result.ignored_lines;
      } else {
        block.country = std::string(country);
        blocks_.push_back(std::move(block));
        ++parsed;
      }
    }
    if (newline == std::string_view::npos) break;
    offset = newline + 1U;
  }
  normalize();
  result.inserted_blocks = blocks_.size() - before;
  result.duplicate_blocks = parsed - result.inserted_blocks;
  // The last import that actually contributed a block defines the origin of
  // the contents, exactly as it does for the Morse alphabet: a host clears the
  // table before loading a file, and the compiled-in copy is only ever
  // imported into a table that is still empty.
  if (result.inserted_blocks > 0) source_ = source;
  return result;
}

void CwCallsignPrefixTable::normalize() {
  std::stable_sort(blocks_.begin(), blocks_.end(),
                   [](const Block& left, const Block& right) {
                     return compareBounds(left.start, right.start) < 0;
                   });
  // A stable sort keeps import order among equal start bounds, so erasing the
  // later ones leaves the file's first spelling in place.
  blocks_.erase(std::unique(blocks_.begin(), blocks_.end(),
                            [](const Block& left, const Block& right) {
                              return compareBounds(left.start,
                                                   right.start) == 0;
                            }),
                blocks_.end());
  Bound reach{};
  for (std::size_t index = 0; index < blocks_.size(); ++index) {
    if (index == 0 || compareBounds(blocks_[index].end, reach) > 0) {
      reach = blocks_[index].end;
    }
    blocks_[index].reach = reach;
  }
}

const CwCallsignPrefixTable::Block* CwCallsignPrefixTable::findBlock(
    const std::string_view callsign) const {
  // A heard token is not always bare. Strip a DX cluster node suffix first:
  // VE7CC-1 is VE7CC with the node number the cluster appends, and no callsign
  // contains a hyphen.
  auto token = callsign.substr(0, callsign.find('-'));

  // Then the portable form. DL/W1AW and IU0LFQ/P put the interesting element
  // on opposite sides of the slash, and the rule that reads both correctly is:
  // if the element before the slash is itself an allocated prefix, that is the
  // country -- otherwise try the element after it.
  //
  // This is right for both by construction. In DL/W1AW the leading DL is the
  // country the station is operating from, and it is allocated, so it wins. In
  // IU0LFQ/P the leading element is the whole callsign, also allocated, so it
  // wins too and the trailing /P is simply never consulted. The fallback to
  // the element after the slash is what rescues a token whose leading element
  // is not a prefix at all, such as a bare /P that lost its callsign to a
  // fade. Note that the answer here is only "which country", never "which
  // DXCC entity": W1AW/KH6 resolves to the United States rather than Hawaii,
  // and for the one question this table answers that costs nothing.
  const auto slash = token.find('/');
  if (slash != std::string_view::npos) {
    const auto leading = token.substr(0, slash);
    if (const auto* block = findBlockForElement(leading); block != nullptr) {
      return block;
    }
    const auto trailing = token.substr(slash + 1U);
    token = trailing.substr(0, trailing.find('/'));
  }
  return findBlockForElement(token);
}

const CwCallsignPrefixTable::Block*
CwCallsignPrefixTable::findBlockForElement(
    const std::string_view element) const {
  if (element.empty() || blocks_.empty()) return nullptr;
  // Longest first, so the most specific allocation applies. T77XX must reach
  // T7A-T7Z as San Marino rather than falling through to Turkiye's TAA-TCZ,
  // and 3DA0AB must reach Eswatini rather than Fiji. Trying the shortest
  // prefix first would answer with the wrong country for every administration
  // that shares an opening letter with a shorter block.
  for (std::size_t length = 3; length >= 1; --length) {
    const auto candidate = paddedPrefix(element, length);
    // The last block that starts at or below the candidate.
    const auto upper_block = std::upper_bound(
        blocks_.begin(), blocks_.end(), candidate,
        [](const Bound& probe, const Block& block) {
          return compareBounds(probe, block.start) < 0;
        });
    for (auto it = upper_block; it != blocks_.begin();) {
      --it;
      if (compareBounds(candidate, it->end) <= 0) return &*it;
      // No block at or before this one reaches the candidate, so no earlier
      // block can contain it either. The shipped file's blocks do not overlap
      // and this stops immediately; the walk exists so that an operator who
      // adds a narrow block inside a wider one does not lose the wider one's
      // cover for the prefixes around it.
      if (compareBounds(it->reach, candidate) < 0) break;
    }
  }
  return nullptr;
}

bool CwCallsignPrefixTable::isAllocatedPrefix(
    const std::string_view callsign) const {
  // A table nothing was imported into has no opinion. Answering false here
  // would refuse every station on the band at once, which is far worse than
  // the misdecodes this check exists to catch, so an empty table admits
  // everything and leaves the judgement to whatever else is looking.
  if (blocks_.empty()) return true;
  return findBlock(callsign) != nullptr;
}

std::string_view CwCallsignPrefixTable::countryFor(
    const std::string_view callsign) const {
  const auto* block = findBlock(callsign);
  return block == nullptr ? std::string_view{} : block->country;
}

std::size_t CwCallsignPrefixTable::size() const noexcept {
  return blocks_.size();
}

bool CwCallsignPrefixTable::empty() const noexcept { return blocks_.empty(); }

CwCallsignPrefixSource CwCallsignPrefixTable::source() const noexcept {
  return source_;
}

void CwCallsignPrefixTable::clear() noexcept {
  blocks_.clear();
  // An emptied table has no origin. Leaving the previous one in place would
  // let a host that clears and then fails to load report the origin of blocks
  // that are no longer there.
  source_ = CwCallsignPrefixSource::kNone;
}

CwCallsignPrefixTable& cwMutableSharedCallsignPrefixes() noexcept {
  static CwCallsignPrefixTable table;
  return table;
}

namespace {

// The fallback chain the shared table recovers through, performed on a table
// the caller has already found empty.
//
// noexcept because one of its two callers is declared noexcept and the other
// is reached from noexcept decode paths: a prefix file that cannot be read
// has to stay a failure to load rather than become a terminate.
void loadSharedCallsignPrefixes(CwCallsignPrefixTable& table) noexcept {
  try {
    const char* directory = std::getenv("CWA_DICTIONARY_DIR");
    if (directory != nullptr) {
      std::ifstream input(std::string(directory) + "/callsign-prefixes.txt",
                          std::ios::binary);
      if (input) {
        std::ostringstream buffer;
        buffer << input.rdbuf();
        static_cast<void>(table.importText(buffer.str()));
      }
    }
  } catch (...) {
    // A read that threw part way through leaves a partial table behind with
    // the loaded origin recorded against it. Discard it so the compiled-in
    // copy below takes over whole and names itself as the origin.
    table.clear();
  }
  if (!table.empty()) return;
  // Last resort, and the reason the compiled-in copy exists at all. A missing
  // or unreadable file must not be allowed to answer "no such country" for
  // every callsign at once -- that would be a far larger fault than the
  // invented prefixes this table is here to catch. The text below is
  // generated from the same shipped file at build time, so this is not a
  // second table to keep in step: it is the same one, compiled in.
  try {
    static_cast<void>(table.importText(kCwBuiltinCallsignPrefixes,
                                       CwCallsignPrefixSource::kBuiltin));
  } catch (...) {
    table.clear();
  }
}

// Loads the shared table if nothing has loaded one yet.
//
// The first use is behind C++ thread-safe static initialization, because it
// is the only one that can happen on several threads at once: a host that
// never loaded the table eagerly has every decode thread arriving at an empty
// table together, and the plain "if empty, import" this replaced had them all
// building the same std::vector at the same time.
void ensureSharedCallsignPrefixesLoaded() noexcept {
  auto& table = cwMutableSharedCallsignPrefixes();
  static const bool attempted = [&table]() noexcept {
    if (table.empty()) loadSharedCallsignPrefixes(table);
    return true;
  }();
  static_cast<void>(attempted);
  // A later clear() still recovers. The application clears the table before
  // importing the operator's file, and a test does the same to exercise the
  // fallback, so a once-and-never-again guard would hand both an empty table
  // -- which admits every invented prefix on the band. Unlike first use, this
  // path is not concurrent by construction: it can only follow a clear() on
  // the shared table, which is already an exclusive operation on it.
  if (table.empty()) loadSharedCallsignPrefixes(table);
}

}  // namespace

const CwCallsignPrefixTable& cwSharedCallsignPrefixes() {
  ensureSharedCallsignPrefixesLoaded();
  return cwMutableSharedCallsignPrefixes();
}

bool cwCallsignPrefixesLoadedFromBuiltin() noexcept {
  // Asking where the table came from has to answer for the table the next
  // lookup will actually use, so it loads one the same way that lookup would.
  // Reading the recorded origin without doing so answered "not the
  // compiled-in copy" while nothing was loaded at all -- which is the answer
  // a perfectly healthy file load gives, and therefore precisely the
  // backwards reading this flag exists to prevent.
  ensureSharedCallsignPrefixesLoaded();
  return cwMutableSharedCallsignPrefixes().source() ==
      CwCallsignPrefixSource::kBuiltin;
}

}  // namespace cwassistant::core
