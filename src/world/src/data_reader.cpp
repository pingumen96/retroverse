#include "data_reader.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#include <rv/assets/prefab_manifest.hpp>
#include <rv/core/file_io.hpp>

namespace rv::world::detail {
namespace {

/// Deeper documents are not scanned for lines (the world's documents nest a few levels).
constexpr std::size_t kMaxScanDepth = 256;

/// Walks a JSON text (comments allowed) as nlohmann::json reads it, keeping the line of every value: finds the line of
/// a path, or the first key that appears twice in one object. The text has parsed already, so the walker only needs to
/// be right on valid input; on anything it does not expect it stops and reports nothing.
class Walker {
public:
    explicit Walker(std::string_view text) : m_text(text) {}

    /// The line of the deepest value on the path `target` (tokens unescaped), or nothing when the text cannot be
    /// walked.
    std::optional<std::size_t> locate(const std::vector<std::string>& target) {
        m_target = &target;
        m_bestLine = 0;
        if (!walkDocument() && m_bestLine == 0) {
            return std::nullopt;
        }
        return m_bestLine;
    }

    std::optional<DuplicateKey> duplicate() {
        m_detectDuplicates = true;
        walkDocument();
        return m_duplicate;
    }

private:
    bool walkDocument() {
        // nlohmann::json skips a UTF-8 byte order mark; so does the walker.
        m_pos = m_text.starts_with("\xEF\xBB\xBF") ? 3 : 0;
        m_line = 1;
        m_stop = false;
        return skipSpace() && value(0, 0);
    }

    bool atEnd() const noexcept { return m_pos >= m_text.size(); }
    char peek() const noexcept { return m_text[m_pos]; }

    /// Skips white space and comments, counting lines. False on a comment that does not end.
    bool skipSpace() {
        while (!atEnd()) {
            const char c = peek();
            if (c == '\n') {
                ++m_line;
                ++m_pos;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                ++m_pos;
            } else if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '/') {
                while (!atEnd() && peek() != '\n') {
                    ++m_pos;
                }
            } else if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '*') {
                const std::size_t end = m_text.find("*/", m_pos + 2);
                if (end == std::string_view::npos) {
                    return false;
                }
                m_line += static_cast<std::size_t>(std::count(m_text.begin() + static_cast<std::ptrdiff_t>(m_pos),
                                                              m_text.begin() + static_cast<std::ptrdiff_t>(end), '\n'));
                m_pos = end + 2;
            } else {
                return true;
            }
        }
        return true;
    }

    static void appendUtf8(std::string& out, std::uint32_t code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool hex4(std::uint32_t& code) {
        if (m_pos + 4 > m_text.size()) {
            return false;
        }
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_pos++];
            code <<= 4;
            if (c >= '0' && c <= '9') {
                code |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                code |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                code |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    /// Reads a string token (at its opening quote) into `out`, its escapes decoded as JSON defines them.
    bool string(std::string& out) {
        ++m_pos;
        while (!atEnd()) {
            const char c = m_text[m_pos++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (atEnd()) {
                return false;
            }
            const char escape = m_text[m_pos++];
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                out += escape;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                std::uint32_t code = 0;
                if (!hex4(code)) {
                    return false;
                }
                if (code >= 0xD800 && code <= 0xDBFF && m_pos + 1 < m_text.size() && m_text[m_pos] == '\\' &&
                    m_text[m_pos + 1] == 'u') {
                    m_pos += 2;
                    std::uint32_t low = 0;
                    if (!hex4(low)) {
                        return false;
                    }
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                appendUtf8(out, code);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    /// A value at the current position. `matched` counts the target's tokens this value's path matches, or is -1 once
    /// the path left the target.
    bool value(int matched, std::size_t depth) {
        if (atEnd() || depth > kMaxScanDepth) {
            return false;
        }
        if (m_target != nullptr && matched >= 0) {
            m_bestLine = m_line;
            if (static_cast<std::size_t>(matched) == m_target->size()) {
                m_stop = true;
                return true;
            }
        }
        const char c = peek();
        if (c == '{') {
            return object(matched, depth);
        }
        if (c == '[') {
            return array(matched, depth);
        }
        if (c == '"') {
            std::string ignored;
            return string(ignored);
        }
        // A number or a literal: up to the next delimiter.
        const std::size_t start = m_pos;
        while (!atEnd()) {
            const char d = peek();
            if (d == ',' || d == ']' || d == '}' || d == ' ' || d == '\t' || d == '\r' || d == '\n' || d == '/') {
                break;
            }
            ++m_pos;
        }
        return m_pos > start;
    }

    int childMatch(int matched, std::string_view token) const {
        if (m_target == nullptr || matched < 0 || static_cast<std::size_t>(matched) >= m_target->size() ||
            (*m_target)[static_cast<std::size_t>(matched)] != token) {
            return -1;
        }
        return matched + 1;
    }

    std::string currentPointer(std::string_view lastToken) const {
        std::string pointer;
        for (const std::string& token : m_path) {
            pointer = pointerChild(pointer, token);
        }
        return pointerChild(pointer, lastToken);
    }

    bool object(int matched, std::size_t depth) {
        ++m_pos;
        std::set<std::string> keys;
        if (!skipSpace() || atEnd()) {
            return false;
        }
        if (peek() == '}') {
            ++m_pos;
            return true;
        }
        while (true) {
            if (!skipSpace() || atEnd() || peek() != '"') {
                return false;
            }
            const std::size_t keyLine = m_line;
            std::string key;
            if (!string(key) || !skipSpace() || atEnd() || peek() != ':') {
                return false;
            }
            ++m_pos;
            if (m_detectDuplicates && !keys.insert(key).second) {
                m_duplicate = DuplicateKey{currentPointer(key), keyLine};
                m_stop = true;
                return true;
            }
            m_path.push_back(key);
            const bool read = skipSpace() && value(childMatch(matched, key), depth + 1);
            m_path.pop_back();
            if (!read || m_stop) {
                return read;
            }
            if (!skipSpace() || atEnd()) {
                return false;
            }
            if (peek() == ',') {
                ++m_pos;
                continue;
            }
            if (peek() == '}') {
                ++m_pos;
                return true;
            }
            return false;
        }
    }

    bool array(int matched, std::size_t depth) {
        ++m_pos;
        if (!skipSpace() || atEnd()) {
            return false;
        }
        if (peek() == ']') {
            ++m_pos;
            return true;
        }
        for (std::size_t index = 0;; ++index) {
            const std::string token = std::to_string(index);
            m_path.push_back(token);
            const bool read = skipSpace() && value(childMatch(matched, token), depth + 1);
            m_path.pop_back();
            if (!read || m_stop) {
                return read;
            }
            if (!skipSpace() || atEnd()) {
                return false;
            }
            if (peek() == ',') {
                ++m_pos;
                continue;
            }
            if (peek() == ']') {
                ++m_pos;
                return true;
            }
            return false;
        }
    }

    std::string_view m_text;
    std::size_t m_pos = 0;
    std::size_t m_line = 1;
    bool m_stop = false;
    std::vector<std::string> m_path;
    const std::vector<std::string>* m_target = nullptr;
    std::size_t m_bestLine = 0;
    bool m_detectDuplicates = false;
    std::optional<DuplicateKey> m_duplicate;
};

/// The reference tokens of a JSON pointer, unescaped; nothing when it is not one.
std::optional<std::vector<std::string>> pointerTokens(std::string_view pointer) {
    std::vector<std::string> tokens;
    if (pointer.empty()) {
        return tokens;
    }
    if (pointer.front() != '/') {
        return std::nullopt;
    }
    std::string token;
    for (std::size_t i = 1; i <= pointer.size(); ++i) {
        if (i == pointer.size() || pointer[i] == '/') {
            tokens.push_back(std::move(token));
            token.clear();
        } else if (pointer[i] == '~' && i + 1 < pointer.size() && (pointer[i + 1] == '0' || pointer[i + 1] == '1')) {
            token += pointer[i + 1] == '0' ? '~' : '/';
            ++i;
        } else {
            token += pointer[i];
        }
    }
    return tokens;
}

std::size_t lineAtByte(std::string_view text, std::size_t byte) {
    const std::size_t end = std::min(byte, text.size());
    return 1 +
           static_cast<std::size_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(end), '\n'));
}

/// nlohmann's message without its "[json.exception.parse_error.101] " prefix.
std::string_view withoutExceptionTag(std::string_view what) {
    if (what.starts_with('[')) {
        if (const std::size_t end = what.find("] "); end != std::string_view::npos) {
            return what.substr(end + 2);
        }
    }
    return what;
}

} // namespace

std::string pointerChild(std::string_view pointer, std::string_view token) {
    std::string result(pointer);
    result += '/';
    for (const char c : token) {
        if (c == '~') {
            result += "~0";
        } else if (c == '/') {
            result += "~1";
        } else {
            result += c;
        }
    }
    return result;
}

std::string pointerChild(std::string_view pointer, std::size_t index) {
    return pointerChild(pointer, std::to_string(index));
}

std::optional<std::size_t> lineOfPointer(std::string_view text, std::string_view pointer) {
    const auto tokens = pointerTokens(pointer);
    if (!tokens) {
        return std::nullopt;
    }
    return Walker(text).locate(*tokens);
}

std::optional<DuplicateKey> findDuplicateKey(std::string_view text) {
    return Walker(text).duplicate();
}

DocumentReader::DocumentReader(std::string_view text, std::string context)
    : m_text(text), m_context(std::move(context)) {}

void DocumentReader::fail(std::string_view pointer, std::string_view message, ErrorCode code) {
    if (m_failed) {
        return;
    }
    m_failed = true;
    m_code = code;
    m_pointer = pointer;
    m_message = message;
}

Error DocumentReader::error() const {
    Error error;
    error.code = m_code;
    error.message = m_pointer.empty() ? m_message : m_pointer + ": " + m_message;
    error.context = m_context;
    if (const auto line = lineOfPointer(m_text, m_pointer)) {
        error.context += ":" + std::to_string(*line);
    }
    return error;
}

Expected<nlohmann::json> parseDataJson(std::string_view text, std::string_view context) {
    nlohmann::json value;
    try {
        value = nlohmann::json::parse(text.begin(), text.end(), /*callback*/ nullptr, /*allow_exceptions*/ true,
                                      /*ignore_comments*/ true);
    } catch (const nlohmann::json::parse_error& e) {
        return makeError(ErrorCode::ParseError, std::string(withoutExceptionTag(e.what())),
                         std::string(context) + ":" + std::to_string(lineAtByte(text, e.byte == 0 ? 0 : e.byte - 1)));
    } catch (const nlohmann::json::exception& e) {
        // out_of_range (406): a number that overflows a double; nlohmann gives no position for it.
        return makeError(ErrorCode::ParseError, std::string(withoutExceptionTag(e.what())), std::string(context));
    }
    if (const auto duplicate = findDuplicateKey(text)) {
        return makeError(ErrorCode::ValidationFailed, duplicate->pointer + ": the key appears twice in its object",
                         std::string(context) + ":" + std::to_string(duplicate->line));
    }
    return value;
}

Expected<std::string> readDataFile(const std::filesystem::path& path) {
    return core::readTextFile(path, kMaxDataFileBytes);
}

std::string describeType(const nlohmann::json& value) {
    switch (value.type()) {
    case nlohmann::json::value_t::null:
        return "null";
    case nlohmann::json::value_t::boolean:
        return "a boolean";
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned:
        return "an integer";
    case nlohmann::json::value_t::number_float:
        return "a number with a fraction or an exponent";
    case nlohmann::json::value_t::string:
        return "a string";
    case nlohmann::json::value_t::array:
        return "an array";
    case nlohmann::json::value_t::object:
        return "an object";
    default:
        return "an unsupported value";
    }
}

std::int64_t readInteger(DocumentReader& document, const nlohmann::json& value, std::string_view pointer,
                         std::int64_t minimum, std::int64_t maximum) {
    if (document.failed()) {
        return minimum;
    }
    if (!value.is_number_integer()) {
        document.fail(pointer, "expected an integer, found " + describeType(value));
        return minimum;
    }
    const bool tooLarge =
        value.is_number_unsigned() &&
        value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const std::int64_t number = tooLarge ? std::numeric_limits<std::int64_t>::max() : value.get<std::int64_t>();
    if (tooLarge || number < minimum || number > maximum) {
        document.fail(pointer, "expected an integer from " + std::to_string(minimum) + " to " +
                                   std::to_string(maximum) + ", found " + value.dump());
        return minimum;
    }
    return number;
}

std::string readString(DocumentReader& document, const nlohmann::json& value, std::string_view pointer) {
    if (document.failed()) {
        return {};
    }
    if (!value.is_string()) {
        document.fail(pointer, "expected a string, found " + describeType(value));
        return {};
    }
    return value.get<std::string>();
}

ObjectReader::ObjectReader(DocumentReader& document, const nlohmann::json& value, std::string pointer,
                           std::initializer_list<std::string_view> keys)
    : m_document(document), m_pointer(std::move(pointer)) {
    if (m_document.failed()) {
        return;
    }
    if (!value.is_object()) {
        m_document.fail(m_pointer, "expected an object, found " + describeType(value));
        return;
    }
    for (const auto& [key, member] : value.items()) {
        if (std::find(keys.begin(), keys.end(), std::string_view(key)) == keys.end()) {
            m_document.fail(pointerChild(m_pointer, key), "unknown key");
            return;
        }
    }
    m_object = &value;
}

bool ObjectReader::has(std::string_view key) const {
    return m_object != nullptr && m_object->contains(key);
}

const nlohmann::json& ObjectReader::value(std::string_view key) const {
    static const nlohmann::json kNull;
    if (m_document.failed() || m_object == nullptr) {
        return kNull;
    }
    const auto found = m_object->find(key);
    if (found == m_object->end()) {
        m_document.fail(pointer(key), "missing");
        return kNull;
    }
    return *found;
}

std::int64_t ObjectReader::integer(std::string_view key, std::int64_t minimum, std::int64_t maximum) const {
    const nlohmann::json& member = value(key);
    return readInteger(m_document, member, pointer(key), minimum, maximum);
}

std::optional<std::int64_t> ObjectReader::optionalInteger(std::string_view key, std::int64_t minimum,
                                                          std::int64_t maximum) const {
    if (!has(key)) {
        return std::nullopt;
    }
    return integer(key, minimum, maximum);
}

std::uint64_t ObjectReader::unsignedInteger(std::string_view key) const {
    const nlohmann::json& member = value(key);
    if (m_document.failed()) {
        return 0;
    }
    if (!member.is_number_integer() || (!member.is_number_unsigned() && member.get<std::int64_t>() < 0)) {
        m_document.fail(pointer(key), "expected an integer from 0 to 18446744073709551615, found " +
                                          (member.is_number_integer() ? member.dump() : describeType(member)));
        return 0;
    }
    return member.get<std::uint64_t>();
}

std::optional<std::uint64_t> ObjectReader::nullableUnsignedInteger(std::string_view key) const {
    if (value(key).is_null()) {
        return std::nullopt;
    }
    return unsignedInteger(key);
}

std::string ObjectReader::string(std::string_view key) const {
    const nlohmann::json& member = value(key);
    return readString(m_document, member, pointer(key));
}

double ObjectReader::number(std::string_view key, double minimum, double maximum) const {
    const nlohmann::json& member = value(key);
    if (m_document.failed()) {
        return minimum;
    }
    if (!member.is_number()) {
        m_document.fail(pointer(key), "expected a number, found " + describeType(member));
        return minimum;
    }
    const double number = member.get<double>();
    if (!(number >= minimum && number <= maximum)) {
        m_document.fail(pointer(key), "expected a number from " + nlohmann::json(minimum).dump() + " to " +
                                          nlohmann::json(maximum).dump() + ", found " + member.dump());
        return minimum;
    }
    return number;
}

bool ObjectReader::boolean(std::string_view key) const {
    const nlohmann::json& member = value(key);
    if (m_document.failed()) {
        return false;
    }
    if (!member.is_boolean()) {
        m_document.fail(pointer(key), "expected a boolean, found " + describeType(member));
        return false;
    }
    return member.get<bool>();
}

std::optional<bool> ObjectReader::optionalBoolean(std::string_view key) const {
    if (!has(key)) {
        return std::nullopt;
    }
    return boolean(key);
}

const nlohmann::json& ObjectReader::array(std::string_view key) const {
    static const nlohmann::json kEmpty = nlohmann::json::array();
    const nlohmann::json& member = value(key);
    if (m_document.failed()) {
        return kEmpty;
    }
    if (!member.is_array()) {
        m_document.fail(pointer(key), "expected an array, found " + describeType(member));
        return kEmpty;
    }
    return member;
}

ObjectReader ObjectReader::object(std::string_view key, std::initializer_list<std::string_view> keys) const {
    return ObjectReader(m_document, value(key), pointer(key), keys);
}

std::string ObjectReader::dataId(std::string_view key) const {
    return checkedString(key, isValidDataId, "a lowercase id of a-z, 0-9 and '_'");
}

PrefabRef ObjectReader::prefab(std::string_view key) const {
    const ObjectReader reader = object(key, {"kind", "manifest"});
    PrefabRef prefab;
    prefab.manifest = reader.checkedString("manifest", assets::isValidPrefabId, "a prefab manifest id");
    prefab.kind = reader.checkedString("kind", isValidPrefabKind, "a prefab kind of a-z, 0-9 and '_'");
    return prefab;
}

} // namespace rv::world::detail
