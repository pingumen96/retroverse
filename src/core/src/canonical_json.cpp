#include <rv/core/canonical_json.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <fmt/format.h>

#include <rv/core/sha256.hpp>
#include <rv/core/utf8.hpp>

namespace rv::core {
namespace {

using Json = nlohmann::json;

/// The deepest nesting written: a plan nests a few levels; a file edited into thousands would overflow the stack of
/// this recursive writer.
constexpr std::size_t kMaxDepth = 64;

/// `text` in quotes with exactly '"', '\' and the control characters escaped (valid UTF-8 already); the runs between
/// them appended whole.
void appendQuoted(std::string& out, std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    out += '"';
    std::size_t run = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte >= 0x20 && byte != '"' && byte != '\\') {
            continue;
        }
        out.append(text.data() + run, i - run);
        run = i + 1;
        switch (byte) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            out += "\\u00";
            out += kHex[byte >> 4];
            out += kHex[byte & 0x0F];
            break;
        }
    }
    out.append(text.data() + run, text.size() - run);
    out += '"';
}

/// Writes one document; `m_path` holds the tokens down to the value being written, for the pointer of an error.
class CanonicalWriter {
public:
    explicit CanonicalWriter(std::string& out) : m_out(out) {}

    Status document(const Json& value) {
        if (value.is_array()) {
            return arrayValue(value, /*topLevel*/ true);
        }
        if (value.is_object()) {
            return objectValue(value, /*topLevel*/ true);
        }
        return anyValue(value);
    }

    /// `value` as a value nested in a document.
    Status nested(const Json& value) { return anyValue(value); }

private:
    /// One level deeper while alive.
    class Nesting {
    public:
        explicit Nesting(std::size_t& depth) noexcept : m_depth(depth) { ++m_depth; }
        Nesting(const Nesting&) = delete;
        Nesting& operator=(const Nesting&) = delete;
        ~Nesting() { --m_depth; }

    private:
        std::size_t& m_depth;
    };

    Status anyValue(const Json& value) {
        switch (value.type()) {
        case Json::value_t::null:
            m_out += "null";
            return ok();
        case Json::value_t::boolean:
            m_out += value.get<bool>() ? "true" : "false";
            return ok();
        case Json::value_t::number_integer:
            return integer(value.get<std::int64_t>());
        case Json::value_t::number_unsigned:
            return integer(value.get<std::uint64_t>());
        case Json::value_t::string:
            return stringValue(value.get_ref<const std::string&>());
        case Json::value_t::array:
            return arrayValue(value, /*topLevel*/ false);
        case Json::value_t::object:
            return objectValue(value, /*topLevel*/ false);
        case Json::value_t::number_float:
            return refused("a floating-point number (canonical JSON holds integers only)");
        case Json::value_t::binary:
            return refused("a binary value");
        case Json::value_t::discarded:
            break;
        }
        return refused("a discarded value");
    }

    template <class Integer>
    Status integer(Integer value) {
        char digits[24];
        const auto written = std::to_chars(digits, digits + sizeof(digits), value);
        m_out.append(digits, written.ptr);
        return ok();
    }

    Status stringValue(const std::string& text) {
        if (!isValidUtf8(text)) {
            return refused("a string that is not well-formed UTF-8");
        }
        quoted(text);
        return ok();
    }

    void quoted(std::string_view text) { appendQuoted(m_out, text); }

    Status arrayValue(const Json& array, bool topLevel) {
        const Nesting nesting(m_depth);
        if (m_depth > kMaxDepth) {
            return refused("values nested deeper than 64 levels");
        }
        // One element per line in a top-level array that has any; on one line everywhere else.
        const bool lines = topLevel && !array.empty();
        const char* const separator = lines ? ",\n" : ",";
        m_out += lines ? "[\n" : "[";
        std::size_t index = 0;
        for (const Json& element : array) {
            if (index != 0) {
                m_out += separator;
            }
            m_path.push_back(PathToken{nullptr, index});
            if (auto written = anyValue(element); !written) {
                return written;
            }
            m_path.pop_back();
            ++index;
        }
        m_out += lines ? "\n]" : "]";
        return ok();
    }

    Status objectValue(const Json& object, bool topLevel) {
        const Nesting nesting(m_depth);
        if (m_depth > kMaxDepth) {
            return refused("values nested deeper than 64 levels");
        }
        std::vector<std::pair<const std::string*, const Json*>> members;
        members.reserve(object.size());
        for (auto it = object.begin(); it != object.end(); ++it) {
            members.emplace_back(&it.key(), &it.value());
        }
        // Code point order is the order of the UTF-8 bytes, which std::string compares as unsigned char.
        std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) { return *a.first < *b.first; });
        m_out += '{';
        bool first = true;
        for (const auto& [key, member] : members) {
            if (!first) {
                m_out += ',';
            }
            first = false;
            m_path.push_back(PathToken{key, 0});
            if (!isValidUtf8(*key)) {
                return refused("a key that is not well-formed UTF-8");
            }
            quoted(*key);
            m_out += ':';
            // A member of the document's object that is an array is a top-level array.
            const Status written =
                topLevel && member->is_array() ? arrayValue(*member, /*topLevel*/ true) : anyValue(*member);
            if (!written) {
                return written;
            }
            m_path.pop_back();
        }
        m_out += '}';
        return ok();
    }

    /// InvalidArgument naming the value being written by its JSON pointer (RFC 6901: '~' as "~0", '/' as "~1").
    Status refused(std::string_view what) const {
        std::string pointer;
        for (const PathToken& step : m_path) {
            pointer += '/';
            const std::string token = step.key != nullptr ? *step.key : std::to_string(step.index);
            for (const char c : token) {
                if (c == '~') {
                    pointer += "~0";
                } else if (c == '/') {
                    pointer += "~1";
                } else {
                    pointer += c;
                }
            }
        }
        // The keys on the way may be the malformed text being reported: such a pointer is not repeated.
        return makeError(ErrorCode::InvalidArgument, fmt::format("canonical JSON cannot hold {}", what),
                         isValidUtf8(pointer) ? pointer : std::string("(a pointer through a key that is not UTF-8)"));
    }

    /// A step of the pointer: a member's key (owned by the document being written), else an element's index; the
    /// pointer's text is made only for an error (m3/preview-time: a string per value was most of a large write).
    struct PathToken {
        const std::string* key = nullptr;
        std::size_t index = 0;
    };

    std::string& m_out;
    std::vector<PathToken> m_path;
    std::size_t m_depth = 0;
};

} // namespace

Expected<std::string> writeCanonicalJson(const nlohmann::json& value) {
    std::string out;
    CanonicalWriter writer(out);
    if (auto written = writer.document(value); !written) {
        return propagate(std::move(written.error()));
    }
    out += '\n';
    return out;
}

Expected<std::string> canonicalJsonDigest(const nlohmann::json& value) {
    auto text = writeCanonicalJson(value);
    if (!text) {
        return propagate(std::move(text.error()));
    }
    return sha256Hex(*text);
}

Expected<nlohmann::json> parseCanonicalJson(std::string_view text, std::string_view context) {
    nlohmann::json value;
    try {
        value = nlohmann::json::parse(text.begin(), text.end(), /*callback*/ nullptr, /*allow_exceptions*/ true,
                                      /*ignore_comments*/ false);
    } catch (const nlohmann::json::exception& e) {
        // parse_error, and out_of_range for number literals that overflow a double (see parseJson()).
        return makeError(ErrorCode::ParseError, e.what(), std::string(context));
    }
    const auto canonical = writeCanonicalJson(value);
    if (!canonical) {
        return makeError(ErrorCode::ValidationFailed,
                         fmt::format("not canonical JSON: {} (at {})", canonical.error().message,
                                     canonical.error().context.empty() ? "the document" : canonical.error().context),
                         std::string(context));
    }
    if (*canonical != text) {
        const auto departs = std::mismatch(text.begin(), text.end(), canonical->begin(), canonical->end());
        return makeError(ErrorCode::ValidationFailed,
                         fmt::format("not canonical JSON: the text departs from the canonical form at byte {}",
                                     departs.first - text.begin()),
                         std::string(context));
    }
    return value;
}

void CanonicalTextWriter::fail(std::string message) {
    if (!m_error) {
        m_error = makeError(ErrorCode::InvalidArgument, std::move(message), "canonical text").value();
    }
}

bool CanonicalTextWriter::beforeValue() {
    if (m_frames.empty()) {
        if (m_started) {
            fail("a second value after the document");
            return false;
        }
        m_started = true;
        return true;
    }
    Frame& frame = m_frames.back();
    if (frame.object) {
        if (!frame.valueDue) {
            fail("a value where a key is due (after \"" + frame.lastKey + "\")");
            return false;
        }
        frame.valueDue = false;
        return true;
    }
    if (frame.count == 0) {
        if (frame.lines) {
            m_out += '\n';
        }
    } else {
        m_out += frame.lines ? ",\n" : ",";
    }
    ++frame.count;
    return true;
}

void CanonicalTextWriter::beginObject() {
    if (!beforeValue()) {
        return;
    }
    m_out += '{';
    m_frames.push_back(Frame{true, false, 0, {}, false});
    if (m_frames.size() > kMaxDepth) {
        fail("values nested deeper than 64 levels");
    }
}

void CanonicalTextWriter::endObject() {
    if (m_frames.empty() || !m_frames.back().object || m_frames.back().valueDue) {
        fail("an object ended where none is open, or before its last member's value");
        return;
    }
    m_frames.pop_back();
    m_out += '}';
}

void CanonicalTextWriter::beginArray() {
    // One element per line in a top-level array: the document, or a member of the document's object.
    const bool lines = m_frames.empty() || (m_frames.size() == 1 && m_frames.front().object);
    if (!beforeValue()) {
        return;
    }
    m_out += '[';
    m_frames.push_back(Frame{false, lines, 0, {}, false});
    if (m_frames.size() > kMaxDepth) {
        fail("values nested deeper than 64 levels");
    }
}

void CanonicalTextWriter::endArray() {
    if (m_frames.empty() || m_frames.back().object) {
        fail("an array ended where none is open");
        return;
    }
    const bool lines = m_frames.back().lines && m_frames.back().count > 0;
    m_frames.pop_back();
    m_out += lines ? "\n]" : "]";
}

void CanonicalTextWriter::key(std::string_view name) {
    if (m_frames.empty() || !m_frames.back().object || m_frames.back().valueDue) {
        fail("a key where none is due (\"" + std::string(name) + "\")");
        return;
    }
    Frame& frame = m_frames.back();
    // Code point order is the order of the UTF-8 bytes, which std::string compares as unsigned char.
    if (frame.count > 0 && !(frame.lastKey < name)) {
        fail("the key \"" + std::string(name) + "\" after \"" + frame.lastKey + "\" (keys in ascending order, once)");
        return;
    }
    if (!isValidUtf8(name)) {
        fail("a key that is not well-formed UTF-8");
        return;
    }
    if (frame.count > 0) {
        m_out += ',';
    }
    ++frame.count;
    frame.lastKey.assign(name);
    frame.valueDue = true;
    appendQuoted(m_out, name);
    m_out += ':';
}

void CanonicalTextWriter::string(std::string_view text) {
    if (!beforeValue()) {
        return;
    }
    if (!isValidUtf8(text)) {
        fail("a string that is not well-formed UTF-8");
        return;
    }
    appendQuoted(m_out, text);
}

void CanonicalTextWriter::integer(std::int64_t value) {
    if (!beforeValue()) {
        return;
    }
    char digits[24];
    const auto written = std::to_chars(digits, digits + sizeof(digits), value);
    m_out.append(digits, written.ptr);
}

void CanonicalTextWriter::unsignedInteger(std::uint64_t value) {
    if (!beforeValue()) {
        return;
    }
    char digits[24];
    const auto written = std::to_chars(digits, digits + sizeof(digits), value);
    m_out.append(digits, written.ptr);
}

void CanonicalTextWriter::boolean(bool value) {
    if (beforeValue()) {
        m_out += value ? "true" : "false";
    }
}

void CanonicalTextWriter::null() {
    if (beforeValue()) {
        m_out += "null";
    }
}

void CanonicalTextWriter::value(const nlohmann::json& value) {
    if (!beforeValue()) {
        return;
    }
    CanonicalWriter writer(m_out);
    if (Status written = writer.nested(value); !written && !m_error) {
        m_error = written.error();
    }
}

void CanonicalTextWriter::raw(std::string_view text) {
    if (beforeValue()) {
        m_out += text;
    }
}

Status CanonicalTextWriter::finish() {
    if (!m_error && (!m_frames.empty() || !m_started)) {
        fail("an unbalanced document");
    }
    if (m_error) {
        return tl::unexpected<Error>(*m_error);
    }
    m_out += '\n';
    return ok();
}

Expected<std::string> nestedCanonicalText(const nlohmann::json& value) {
    std::string out;
    CanonicalWriter writer(out);
    if (auto written = writer.nested(value); !written) {
        return propagate(std::move(written.error()));
    }
    return out;
}

} // namespace rv::core
