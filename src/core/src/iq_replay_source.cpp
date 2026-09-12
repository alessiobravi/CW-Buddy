#include "cwassistant/core/iq_replay_source.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

namespace cwassistant::core {
namespace {

// A sidecar this application writes nests three levels deep (document,
// "captures" array, segment object). The limit exists only so a pathological
// file cannot recurse the parser into the stack.
constexpr std::size_t kMaximumJsonDepth = 32;

// The inverse of IqWriter's little-endian writers. Reading byte by byte keeps
// a capture made on one host readable on another, which is the reason the
// writer serializes that way in the first place.
std::uint16_t read_u16_le(const char* source) noexcept {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(static_cast<unsigned char>(source[0])) |
      static_cast<std::uint16_t>(
          static_cast<std::uint16_t>(static_cast<unsigned char>(source[1]))
          << 8U));
}

std::uint32_t read_u32_le(const char* source) noexcept {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(source[0])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(source[1]))
          << 8U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(source[2]))
          << 16U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(source[3]))
          << 24U);
}

// Exact inverse of IqWriter's to_i16(), which scales by 32767 rather than
// 32768 so that the two rails stay equidistant from zero. Dividing by 32768
// here would shrink every recorded sample by one part in 32768 -- inaudible,
// invisible on a spectrum, and wrong in a way no listening test would catch.
float from_i16(const std::int16_t value) noexcept {
  return static_cast<float>(value) / 32'767.0F;
}

// A recorded float is passed through unchanged. Clamping it to unity the way
// the WAV path does would discard exactly the over-unity samples a cf32
// capture exists to preserve -- a front end driven into compression is the
// thing being investigated, not noise to be hidden. A non-finite sample is
// still replaced: it cannot be a measurement, and it would poison every
// downstream average it reached.
float from_f32(const std::uint32_t bits) noexcept {
  const float value = std::bit_cast<float>(bits);
  return std::isfinite(value) ? value : 0.0F;
}

// Nanoseconds at an absolute sample index. Computed from the index rather
// than accumulated per block so a long recording cannot drift, and in integer
// arithmetic for the integral rates every real receiver reports: the division
// is split so neither half can overflow, where a single index * 1e9 would
// wrap after a few days of samples. Non-integral rates fall back to floating
// point, which is the best that can be done for them anyway.
std::uint64_t sample_index_to_nanoseconds(const std::uint64_t index,
                                          const double sample_rate_hz) noexcept {
  constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
  if (sample_rate_hz >= 1.0 &&
      sample_rate_hz <= static_cast<double>(std::numeric_limits<
                                            std::uint32_t>::max()) &&
      sample_rate_hz == std::floor(sample_rate_hz)) {
    const auto rate = static_cast<std::uint64_t>(sample_rate_hz);
    const std::uint64_t whole_seconds = index / rate;
    const std::uint64_t sample_remainder = index % rate;
    return whole_seconds * kNanosecondsPerSecond +
           sample_remainder * kNanosecondsPerSecond / rate;
  }
  if (!(sample_rate_hz > 0.0)) return 0;
  return static_cast<std::uint64_t>(static_cast<long double>(index) *
                                    1'000'000'000.0L /
                                    static_cast<long double>(sample_rate_hz));
}

// A minimal JSON value. Object members keep insertion order in a vector
// because a sidecar has a handful of keys and a map would cost an allocation
// per field for no measurable lookup gain.
struct JsonValue {
  enum class Type { Null, Boolean, Number, String, Array, Object };

  Type type{Type::Null};
  bool boolean{false};
  double number{0.0};
  std::string text;
  // An array holds its elements here; an object holds its values here and the
  // matching names alongside. Two parallel vectors rather than one vector of
  // pairs because std::vector is the only container the standard specifies to
  // work with an incomplete element type, and JsonValue is necessarily
  // incomplete inside its own definition.
  std::vector<JsonValue> elements;
  std::vector<std::string> member_names;

  [[nodiscard]] const JsonValue* member(
      const std::string_view key) const noexcept {
    if (type != Type::Object) return nullptr;
    for (std::size_t index = 0; index < member_names.size(); ++index) {
      if (member_names[index] == key) return &elements[index];
    }
    return nullptr;
  }
};

// Strict recursive-descent JSON, sufficient for a SigMF sidecar and nothing
// more. Strict is the point: a scanner that merely hunted for `"core:
// sample_rate"` and read digits after it would happily accept a half-written
// file, a file with the field inside a comment-like string, or a file whose
// structure says something entirely different from what was extracted -- and
// would then replay a recording at a rate nobody ever recorded it at. Every
// deviation from RFC 8259 is refused with the byte offset that caused it.
class JsonParser {
 public:
  explicit JsonParser(const std::string_view text) noexcept : text_(text) {}

  [[nodiscard]] bool parse(JsonValue& out) {
    skipWhitespace();
    if (!parseValue(out, 0)) return false;
    skipWhitespace();
    if (position_ != text_.size()) {
      return fail("trailing content after the top-level value");
    }
    return true;
  }

  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  [[nodiscard]] bool fail(const std::string_view reason) {
    if (error_.empty()) {
      error_.assign(reason);
      error_.append(" at byte ");
      error_.append(std::to_string(position_));
    }
    return false;
  }

  [[nodiscard]] bool atEnd() const noexcept {
    return position_ >= text_.size();
  }

  [[nodiscard]] char peek() const noexcept { return text_[position_]; }

  void skipWhitespace() noexcept {
    while (!atEnd()) {
      const char character = text_[position_];
      // RFC 8259 whitespace exactly: no comments, no stray control bytes.
      if (character != ' ' && character != '\t' && character != '\n' &&
          character != '\r') {
        break;
      }
      ++position_;
    }
  }

  [[nodiscard]] bool expect(const char character) {
    if (atEnd() || peek() != character) {
      std::string reason("expected '");
      reason.push_back(character);
      reason.push_back('\'');
      return fail(reason);
    }
    ++position_;
    return true;
  }

  [[nodiscard]] bool parseLiteral(const std::string_view literal) {
    if (text_.compare(position_, literal.size(), literal) != 0) {
      return fail("unknown literal");
    }
    position_ += literal.size();
    return true;
  }

  [[nodiscard]] bool parseValue(JsonValue& out, const std::size_t depth) {
    if (depth > kMaximumJsonDepth) return fail("nesting is too deep");
    if (atEnd()) return fail("unexpected end of document");
    switch (peek()) {
      case '{':
        return parseObject(out, depth);
      case '[':
        return parseArray(out, depth);
      case '"':
        out.type = JsonValue::Type::String;
        return parseString(out.text);
      case 't':
        out.type = JsonValue::Type::Boolean;
        out.boolean = true;
        return parseLiteral("true");
      case 'f':
        out.type = JsonValue::Type::Boolean;
        out.boolean = false;
        return parseLiteral("false");
      case 'n':
        out.type = JsonValue::Type::Null;
        return parseLiteral("null");
      default:
        return parseNumber(out);
    }
  }

  [[nodiscard]] bool parseObject(JsonValue& out, const std::size_t depth) {
    out.type = JsonValue::Type::Object;
    if (!expect('{')) return false;
    skipWhitespace();
    if (!atEnd() && peek() == '}') {
      ++position_;
      return true;
    }
    for (;;) {
      skipWhitespace();
      std::string key;
      if (atEnd() || peek() != '"') return fail("expected a member name");
      if (!parseString(key)) return false;
      // Duplicate keys are legal JSON and ambiguous for a reader: two
      // "core:sample_rate" members mean the file states two different rates,
      // and picking either one silently is a guess.
      for (const auto& name : out.member_names) {
        if (name == key) return fail("duplicate member name");
      }
      skipWhitespace();
      if (!expect(':')) return false;
      skipWhitespace();
      JsonValue value;
      if (!parseValue(value, depth + 1)) return false;
      out.member_names.push_back(std::move(key));
      out.elements.push_back(std::move(value));
      skipWhitespace();
      if (atEnd()) return fail("unterminated object");
      if (peek() == ',') {
        ++position_;
        continue;
      }
      if (peek() == '}') {
        ++position_;
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }

  [[nodiscard]] bool parseArray(JsonValue& out, const std::size_t depth) {
    out.type = JsonValue::Type::Array;
    if (!expect('[')) return false;
    skipWhitespace();
    if (!atEnd() && peek() == ']') {
      ++position_;
      return true;
    }
    for (;;) {
      skipWhitespace();
      JsonValue value;
      if (!parseValue(value, depth + 1)) return false;
      out.elements.push_back(std::move(value));
      skipWhitespace();
      if (atEnd()) return fail("unterminated array");
      if (peek() == ',') {
        ++position_;
        continue;
      }
      if (peek() == ']') {
        ++position_;
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  [[nodiscard]] bool parseHex4(std::uint32_t& out) {
    if (position_ + 4 > text_.size()) return fail("truncated \\u escape");
    out = 0;
    for (int index = 0; index < 4; ++index) {
      const char digit = text_[position_ + static_cast<std::size_t>(index)];
      std::uint32_t nibble = 0;
      if (digit >= '0' && digit <= '9') {
        nibble = static_cast<std::uint32_t>(digit - '0');
      } else if (digit >= 'a' && digit <= 'f') {
        nibble = static_cast<std::uint32_t>(digit - 'a') + 10U;
      } else if (digit >= 'A' && digit <= 'F') {
        nibble = static_cast<std::uint32_t>(digit - 'A') + 10U;
      } else {
        return fail("invalid \\u escape");
      }
      out = (out << 4U) | nibble;
    }
    position_ += 4;
    return true;
  }

  void appendUtf8(std::string& out, const std::uint32_t code_point) {
    if (code_point < 0x80U) {
      out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800U) {
      out.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
      out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point < 0x1'0000U) {
      out.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
      out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
      out.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
      out.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
  }

  [[nodiscard]] bool parseString(std::string& out) {
    if (!expect('"')) return false;
    out.clear();
    for (;;) {
      if (atEnd()) return fail("unterminated string");
      const char character = text_[position_];
      if (character == '"') {
        ++position_;
        return true;
      }
      if (static_cast<unsigned char>(character) < 0x20U) {
        return fail("unescaped control character in a string");
      }
      if (character != '\\') {
        out.push_back(character);
        ++position_;
        continue;
      }
      ++position_;
      if (atEnd()) return fail("unterminated escape");
      const char escape = text_[position_++];
      switch (escape) {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          std::uint32_t code_point = 0;
          if (!parseHex4(code_point)) return false;
          if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
            // A high surrogate is only meaningful paired; alone it cannot be
            // encoded as UTF-8 at all, so it is a malformed document rather
            // than a character this reader chooses not to support.
            if (position_ + 1 >= text_.size() || text_[position_] != '\\' ||
                text_[position_ + 1] != 'u') {
              return fail("unpaired UTF-16 surrogate");
            }
            position_ += 2;
            std::uint32_t low = 0;
            if (!parseHex4(low)) return false;
            if (low < 0xDC00U || low > 0xDFFFU) {
              return fail("unpaired UTF-16 surrogate");
            }
            code_point = 0x1'0000U + ((code_point - 0xD800U) << 10U) +
                         (low - 0xDC00U);
          } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
            return fail("unpaired UTF-16 surrogate");
          }
          appendUtf8(out, code_point);
          break;
        }
        default:
          return fail("unknown string escape");
      }
    }
  }

  [[nodiscard]] bool parseNumber(JsonValue& out) {
    const std::size_t start = position_;
    if (!atEnd() && peek() == '-') ++position_;
    if (atEnd() || peek() < '0' || peek() > '9') return fail("expected a value");
    if (peek() == '0') {
      ++position_;
      // JSON forbids a leading zero. Accepting "007" would also mean
      // accepting whatever a half-written or machine-mangled field became.
      if (!atEnd() && peek() >= '0' && peek() <= '9') {
        return fail("leading zero in a number");
      }
    } else {
      while (!atEnd() && peek() >= '0' && peek() <= '9') ++position_;
    }
    if (!atEnd() && peek() == '.') {
      ++position_;
      if (atEnd() || peek() < '0' || peek() > '9') {
        return fail("missing digits after the decimal point");
      }
      while (!atEnd() && peek() >= '0' && peek() <= '9') ++position_;
    }
    if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
      ++position_;
      if (!atEnd() && (peek() == '+' || peek() == '-')) ++position_;
      if (atEnd() || peek() < '0' || peek() > '9') {
        return fail("missing digits in the exponent");
      }
      while (!atEnd() && peek() >= '0' && peek() <= '9') ++position_;
    }
    // The span is already validated, so strtod is only being used as a
    // correctly-rounded decimal-to-double conversion, never as a parser.
    const std::string literal(text_.substr(start, position_ - start));
    out.type = JsonValue::Type::Number;
    out.number = std::strtod(literal.c_str(), nullptr);
    if (!std::isfinite(out.number)) return fail("number is out of range");
    return true;
  }

  std::string_view text_;
  std::size_t position_{0};
  std::string error_;
};

[[nodiscard]] const JsonValue* numberMember(const JsonValue& object,
                                            const std::string_view key) {
  const JsonValue* value = object.member(key);
  return value != nullptr && value->type == JsonValue::Type::Number ? value
                                                                    : nullptr;
}

[[nodiscard]] std::string stringMember(const JsonValue& object,
                                       const std::string_view key) {
  const JsonValue* value = object.member(key);
  return value != nullptr && value->type == JsonValue::Type::String
             ? value->text
             : std::string();
}

[[nodiscard]] bool booleanMember(const JsonValue& object,
                                 const std::string_view key) {
  const JsonValue* value = object.member(key);
  return value != nullptr && value->type == JsonValue::Type::Boolean &&
         value->boolean;
}

}  // namespace

std::vector<DeviceInfo> IqReplaySource::enumerate() const { return {}; }

std::string IqReplaySource::dataPathFor(const std::string_view path) {
  constexpr std::string_view kMetaExtension = ".sigmf-meta";
  constexpr std::string_view kDataExtension = ".sigmf-data";
  std::string resolved(path);
  if (resolved.size() >= kMetaExtension.size() &&
      resolved.compare(resolved.size() - kMetaExtension.size(),
                       kMetaExtension.size(), kMetaExtension) == 0) {
    resolved.resize(resolved.size() - kMetaExtension.size());
    resolved.append(kDataExtension);
  }
  return resolved;
}

bool IqReplaySource::open(const std::string_view device_id,
                          const StreamDescriptor& requested) {
  close();
  last_error_.clear();
  if (device_id.empty()) {
    last_error_ = "IQ replay requires a SigMF recording path";
    return false;
  }
  if (requested.kind != StreamKind::ComplexIq) {
    // Handing back an audio-shaped stream would silently discard Q, which is
    // the sideband information the recording was made to keep.
    last_error_ = "IQ replay requires a complex-IQ stream request";
    return false;
  }

  data_path_ = dataPathFor(device_id);
  metadata_path_ = IqWriter::metadataPathFor(data_path_);
  if (!loadMetadata(metadata_path_)) {
    close();
    return false;
  }

  file_.open(data_path_, std::ios::binary);
  if (!file_) {
    last_error_ = "Could not open the SigMF data file";
    close();
    return false;
  }
  file_.seekg(0, std::ios::end);
  const auto end_position = file_.tellg();
  if (!file_ || end_position < 0) {
    last_error_ = "Could not measure the SigMF data file";
    close();
    return false;
  }
  data_bytes_ = static_cast<std::uint64_t>(end_position);

  sample_bytes_ = IqWriter::bytesPerSample(metadata_.format);
  total_samples_ = data_bytes_ / static_cast<std::uint64_t>(sample_bytes_);
  // A capture the operator stopped, or one a power loss ended, can end part
  // way through a sample. Every whole sample before that point is a real
  // measurement; discarding the recording over a few trailing bytes would
  // throw away the entire off-air capture it was made to preserve.
  ends_mid_sample_ =
      (data_bytes_ % static_cast<std::uint64_t>(sample_bytes_)) != 0;
  if (total_samples_ == 0) {
    last_error_ = "SigMF data file contains no complete samples";
    close();
    return false;
  }

  stream_ = {
      .kind = StreamKind::ComplexIq,
      .sample_rate_hz = metadata_.sample_rate_hz,
      .center_frequency_hz = segments_.front().frequency_hz,
      .channel_count = 1,
  };
  byte_buffer_.resize(RealtimeSampleBlock{}.samples.size() * sample_bytes_);
  file_.clear();
  file_.seekg(0, std::ios::beg);
  return static_cast<bool>(file_);
}

bool IqReplaySource::loadMetadata(const std::string& metadata_path) {
  std::ifstream sidecar(metadata_path, std::ios::binary);
  if (!sidecar) {
    last_error_ = "Could not open the SigMF metadata file";
    return false;
  }
  sidecar.seekg(0, std::ios::end);
  const auto end_position = sidecar.tellg();
  if (!sidecar || end_position < 0) {
    last_error_ = "Could not read the SigMF metadata file";
    return false;
  }
  const auto metadata_bytes = static_cast<std::uint64_t>(end_position);
  if (metadata_bytes > kMaximumMetadataBytes) {
    last_error_ = "SigMF metadata file is too large to be a sidecar";
    return false;
  }
  std::string text(static_cast<std::size_t>(metadata_bytes), '\0');
  sidecar.seekg(0, std::ios::beg);
  if (metadata_bytes > 0 &&
      !sidecar.read(text.data(), static_cast<std::streamsize>(metadata_bytes))) {
    last_error_ = "Could not read the SigMF metadata file";
    return false;
  }

  JsonParser parser(text);
  JsonValue document;
  if (!parser.parse(document)) {
    last_error_ = "SigMF metadata is not valid JSON: " + parser.error();
    return false;
  }
  const JsonValue* global = document.member("global");
  if (document.type != JsonValue::Type::Object || global == nullptr ||
      global->type != JsonValue::Type::Object) {
    last_error_ = "SigMF metadata has no \"global\" object";
    return false;
  }

  const JsonValue* datatype = global->member("core:datatype");
  if (datatype == nullptr || datatype->type != JsonValue::Type::String) {
    last_error_ = "SigMF metadata has no \"core:datatype\"";
    return false;
  }
  if (datatype->text == IqWriter::datatypeName(IqSampleFormat::Ci16Le)) {
    metadata_.format = IqSampleFormat::Ci16Le;
  } else if (datatype->text == IqWriter::datatypeName(IqSampleFormat::Cf32Le)) {
    metadata_.format = IqSampleFormat::Cf32Le;
  } else {
    // Guessing here would mean decoding, say, cu8 as ci16: the file would
    // read, the sample count would look plausible, and every sample would be
    // wrong. Name what was found so the operator can convert it.
    last_error_ = "Unsupported SigMF datatype \"" + datatype->text +
                  "\"; this reader handles ci16_le and cf32_le";
    return false;
  }

  const JsonValue* sample_rate = numberMember(*global, "core:sample_rate");
  if (sample_rate == nullptr || !(sample_rate->number > 0.0)) {
    last_error_ = "SigMF metadata has no usable \"core:sample_rate\"";
    return false;
  }
  metadata_.sample_rate_hz = sample_rate->number;

  const JsonValue* captures = document.member("captures");
  if (captures == nullptr || captures->type != JsonValue::Type::Array ||
      captures->elements.empty()) {
    last_error_ = "SigMF metadata has no capture segment";
    return false;
  }
  segments_.clear();
  segments_.reserve(captures->elements.size());
  for (const JsonValue& element : captures->elements) {
    if (element.type != JsonValue::Type::Object) {
      last_error_ = "SigMF capture segment is not an object";
      return false;
    }
    const JsonValue* segment_start = numberMember(element, "core:sample_start");
    const JsonValue* frequency = numberMember(element, "core:frequency");
    if (segment_start == nullptr || !(segment_start->number >= 0.0)) {
      last_error_ = "SigMF capture segment has no \"core:sample_start\"";
      return false;
    }
    if (frequency == nullptr) {
      // The centre frequency is what turns a bin index into an absolute
      // frequency, and therefore what turns a decode into a spot. A capture
      // replayed at an assumed centre would report every station on the wrong
      // frequency while looking entirely healthy.
      last_error_ = "SigMF capture segment has no \"core:frequency\"";
      return false;
    }
    const auto sample_start =
        static_cast<std::uint64_t>(segment_start->number);
    if (segments_.empty()) {
      if (sample_start != 0) {
        last_error_ = "SigMF capture segments do not start at sample 0";
        return false;
      }
    } else if (sample_start <= segments_.back().sample_start) {
      last_error_ = "SigMF capture segments are not in increasing sample order";
      return false;
    }
    segments_.push_back({.sample_start = sample_start,
                         .frequency_hz = frequency->number,
                         .datetime_utc =
                             stringMember(element, "core:datetime")});
  }

  metadata_.center_frequency_hz = segments_.front().frequency_hz;
  metadata_.datetime_utc = segments_.front().datetime_utc;
  metadata_.hardware = stringMember(*global, "core:hw");
  metadata_.description = stringMember(*global, "core:description");
  metadata_.automatic_gain_known =
      booleanMember(*global, "cwbuddy:automatic_gain_known");
  metadata_.automatic_gain = booleanMember(*global, "cwbuddy:automatic_gain");
  const JsonValue* gain = numberMember(*global, "cwbuddy:gain_db");
  metadata_.gain_db = gain != nullptr ? gain->number : 0.0;
  recorded_stop_reason_ = stringMember(*global, "cwbuddy:stop_reason");
  const JsonValue* declared = numberMember(*global, "cwbuddy:sample_count");
  declared_sample_count_ =
      declared != nullptr && declared->number >= 0.0
          ? static_cast<std::uint64_t>(declared->number)
          : 0;
  return true;
}

bool IqReplaySource::start() {
  if (!file_.is_open() || total_samples_ == 0) {
    last_error_ = "No SigMF recording is open";
    return false;
  }
  file_.clear();
  file_.seekg(0, std::ios::beg);
  position_samples_ = 0;
  sequence_ = 0;
  next_segment_ = 1;
  stream_.center_frequency_hz = segments_.front().frequency_hz;
  running_ = static_cast<bool>(file_);
  return running_;
}

void IqReplaySource::stop() noexcept { running_ = false; }

bool IqReplaySource::read(RealtimeSampleBlock& destination,
                          const std::chrono::milliseconds timeout) {
  static_cast<void>(timeout);
  if (!running_ || position_samples_ >= total_samples_) return false;

  std::uint64_t limit = total_samples_;
  // A block carries one centre frequency. Ending it at a retune keeps every
  // sample in it described by the frequency it was actually received on,
  // which is also how the live path behaves: a retune starts a new block.
  if (next_segment_ < segments_.size()) {
    limit = std::min(limit, segments_[next_segment_].sample_start);
  }
  const std::uint64_t remaining = limit - position_samples_;
  const auto wanted = static_cast<std::size_t>(
      std::min<std::uint64_t>(remaining, destination.samples.size()));
  const std::size_t byte_count = wanted * sample_bytes_;
  file_.read(byte_buffer_.data(), static_cast<std::streamsize>(byte_count));
  const auto bytes_read = static_cast<std::size_t>(file_.gcount());
  // The file may have shrunk, or its length may have been measured across a
  // still-running capture. Either way, only whole samples are handed on.
  const std::size_t complete_samples = bytes_read / sample_bytes_;
  if (complete_samples == 0) {
    running_ = false;
    return false;
  }

  destination.stream = stream_;
  destination.sequence = sequence_++;
  destination.timestamp_ns =
      sample_index_to_nanoseconds(position_samples_, stream_.sample_rate_hz);
  destination.sample_count = complete_samples;
  const bool complex_float = metadata_.format == IqSampleFormat::Cf32Le;
  for (std::size_t index = 0; index < complete_samples; ++index) {
    const char* const source = byte_buffer_.data() + index * sample_bytes_;
    if (complex_float) {
      destination.samples[index] = {from_f32(read_u32_le(source)),
                                    from_f32(read_u32_le(source +
                                                         sizeof(float)))};
    } else {
      destination.samples[index] = {
          from_i16(static_cast<std::int16_t>(read_u16_le(source))),
          from_i16(static_cast<std::int16_t>(
              read_u16_le(source + sizeof(std::int16_t))))};
    }
  }
  position_samples_ += complete_samples;

  while (next_segment_ < segments_.size() &&
         segments_[next_segment_].sample_start <= position_samples_) {
    stream_.center_frequency_hz = segments_[next_segment_].frequency_hz;
    ++next_segment_;
  }
  if (position_samples_ >= total_samples_) running_ = false;
  return true;
}

const StreamDescriptor& IqReplaySource::stream_descriptor() const noexcept {
  return stream_;
}

const IqCaptureMetadata& IqReplaySource::capture_metadata() const noexcept {
  return metadata_;
}

const std::vector<IqCaptureSegment>& IqReplaySource::capture_segments()
    const noexcept {
  return segments_;
}

const std::string& IqReplaySource::recorded_stop_reason() const noexcept {
  return recorded_stop_reason_;
}

std::uint64_t IqReplaySource::declared_sample_count() const noexcept {
  return declared_sample_count_;
}

bool IqReplaySource::ends_mid_sample() const noexcept {
  return ends_mid_sample_;
}

std::uint64_t IqReplaySource::total_frames() const noexcept {
  return total_samples_;
}

std::uint64_t IqReplaySource::position_frames() const noexcept {
  return position_samples_;
}

double IqReplaySource::duration_seconds() const noexcept {
  return stream_.sample_rate_hz > 0.0
             ? static_cast<double>(total_samples_) / stream_.sample_rate_hz
             : 0.0;
}

const std::string& IqReplaySource::last_error() const noexcept {
  return last_error_;
}

void IqReplaySource::close() noexcept {
  if (file_.is_open()) file_.close();
  running_ = false;
  stream_ = kNoStream;
  metadata_ = {};
  segments_.clear();
  recorded_stop_reason_.clear();
  byte_buffer_.clear();
  data_bytes_ = 0;
  declared_sample_count_ = 0;
  total_samples_ = 0;
  position_samples_ = 0;
  sequence_ = 0;
  next_segment_ = 0;
  sample_bytes_ = 0;
  ends_mid_sample_ = false;
}

}  // namespace cwassistant::core
