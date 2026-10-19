#pragma once

// Strict reading of the world's JSON documents (private to rv_world): the data files under assets/world and the
// user's world.json (JSON with comments, written by hand) and the canonical documents (the plan, the selection, the
// configuration). Every reader works on one DocumentReader, which keeps the first problem found and turns it into an
// Error naming the file and line: "<file>:<line>" as the context, "<JSON pointer>: <problem>" as the message. Once a
// problem is recorded the readers go on returning neutral values and record nothing more, so a reader is written as a
// straight sequence of reads and checks the document once at the end.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/expected.hpp>
#include <rv/world/ids.hpp>

namespace rv::world::detail {

/// The largest data file read: the world's data and world.json are a few kilobytes.
inline constexpr std::uintmax_t kMaxDataFileBytes = 1024 * 1024;

/// Appends one RFC 6901 reference token ('~' as "~0", '/' as "~1"): ("/sockets", "id") gives "/sockets/id".
[[nodiscard]] std::string pointerChild(std::string_view pointer, std::string_view token);
[[nodiscard]] std::string pointerChild(std::string_view pointer, std::size_t index);

/// The 1-based line of the value that `pointer` names in `text` (JSON, comments allowed), or of the deepest value on
/// its path that exists (the object a missing key belongs to); 1 for the document. Nothing when the text cannot be
/// scanned. Linear in the text.
[[nodiscard]] std::optional<std::size_t> lineOfPointer(std::string_view text, std::string_view pointer);

/// A key that appears twice in one object of `text` (JSON, comments allowed): its pointer (the second occurrence's)
/// and line. nlohmann::json keeps the last value silently; hand-written files are refused instead.
struct DuplicateKey {
    std::string pointer;
    std::size_t line = 0;
};
[[nodiscard]] std::optional<DuplicateKey> findDuplicateKey(std::string_view text);

/// One document being read: its text (for the line of a problem) and the first problem found.
class DocumentReader {
public:
    /// `context` names the document in errors (usually the file).
    DocumentReader(std::string_view text, std::string context);

    /// Records a problem at `pointer`, unless one is recorded already (the first one is reported).
    void fail(std::string_view pointer, std::string_view message, ErrorCode code = ErrorCode::ValidationFailed);
    [[nodiscard]] bool failed() const noexcept { return m_failed; }

    /// The recorded problem: `code`, "<pointer>: <message>" (just the message for the document itself), context
    /// "<context>:<line>". Only meaningful when failed().
    [[nodiscard]] Error error() const;

    [[nodiscard]] const std::string& context() const noexcept { return m_context; }

private:
    std::string_view m_text;
    std::string m_context;
    bool m_failed = false;
    ErrorCode m_code = ErrorCode::ValidationFailed;
    std::string m_pointer;
    std::string m_message;
};

/// Parses a hand-written document: JSON with comments, no key twice in one object. Errors: ParseError with the line of
/// the syntax error ("<context>:<line>", the column in the message), ValidationFailed for a duplicate key.
[[nodiscard]] Expected<nlohmann::json> parseDataJson(std::string_view text, std::string_view context);

/// Reads a data file whole (at most kMaxDataFileBytes). Errors: those of core::readTextFile().
[[nodiscard]] Expected<std::string> readDataFile(const std::filesystem::path& path);

/// One JSON object of a document: a reader of its members. Construction checks that the value is an object whose keys
/// are all listed in `keys` (anything else is a problem at the first unknown key, in key order); a member is read once
/// by the function of its type, which records a problem when it is missing (for the required reads) or of another type
/// or range. Integers are JSON integers (a number written with a fraction or an exponent is refused, 1.0 included).
class ObjectReader {
public:
    ObjectReader(DocumentReader& document, const nlohmann::json& value, std::string pointer,
                 std::initializer_list<std::string_view> keys);

    [[nodiscard]] bool has(std::string_view key) const;
    [[nodiscard]] std::string pointer(std::string_view key) const { return pointerChild(m_pointer, key); }
    [[nodiscard]] const std::string& pointer() const noexcept { return m_pointer; }
    [[nodiscard]] DocumentReader& document() const noexcept { return m_document; }

    /// The member's value, or null (with a problem recorded) when it is missing.
    [[nodiscard]] const nlohmann::json& value(std::string_view key) const;

    [[nodiscard]] std::int64_t integer(std::string_view key, std::int64_t minimum, std::int64_t maximum) const;
    [[nodiscard]] std::optional<std::int64_t> optionalInteger(std::string_view key, std::int64_t minimum,
                                                              std::int64_t maximum) const;
    /// Any integer from 0 to 2^64 - 1 (seeds).
    [[nodiscard]] std::uint64_t unsignedInteger(std::string_view key) const;
    /// unsignedInteger(), or nothing for null (the member must be there either way).
    [[nodiscard]] std::optional<std::uint64_t> nullableUnsignedInteger(std::string_view key) const;
    [[nodiscard]] std::string string(std::string_view key) const;
    /// A JSON number (an integer or one with a fraction) within [minimum, maximum] (lighting profiles, I3 step 16).
    [[nodiscard]] double number(std::string_view key, double minimum, double maximum) const;
    [[nodiscard]] bool boolean(std::string_view key) const;
    [[nodiscard]] std::optional<bool> optionalBoolean(std::string_view key) const;
    /// The member, which must be an array (an empty array is returned with the problem recorded).
    [[nodiscard]] const nlohmann::json& array(std::string_view key) const;
    /// The member as an object reader (one over an empty object when it is missing or not an object).
    [[nodiscard]] ObjectReader object(std::string_view key, std::initializer_list<std::string_view> keys) const;

    /// The member read with `parse` (a function from text to std::optional<T>), `what` naming the expected values in
    /// the problem ("one of north, south, east, west, none").
    template <class T, class Parse>
    [[nodiscard]] T enumeration(std::string_view key, Parse&& parse, std::string_view what) const {
        const std::string text = string(key);
        if (m_document.failed()) {
            return T{};
        }
        if (const std::optional<T> value = parse(std::string_view(text))) {
            return *value;
        }
        m_document.fail(pointer(key), "expected " + std::string(what) + ", found \"" + text + "\"");
        return T{};
    }

    /// The member parsed with `parse` (a function from text to Expected<T>: an id's parse()), `what` naming it in the
    /// problem ("a slot id").
    template <class T, class Parse>
    [[nodiscard]] T parsed(std::string_view key, Parse&& parse, std::string_view what) const {
        const std::string text = string(key);
        if (m_document.failed()) {
            return T{};
        }
        auto value = parse(std::string_view(text));
        if (!value) {
            m_document.fail(pointer(key), "expected " + std::string(what) + ", found \"" + text + "\"");
            return T{};
        }
        return std::move(*value);
    }

    /// A string member that `valid` accepts (a predicate on text), `what` naming the form in the problem.
    template <class Valid>
    [[nodiscard]] std::string checkedString(std::string_view key, Valid&& valid, std::string_view what) const {
        std::string text = string(key);
        if (!m_document.failed() && !valid(std::string_view(text))) {
            m_document.fail(pointer(key), "expected " + std::string(what) + ", found \"" + text + "\"");
        }
        return text;
    }

    /// A data id member (isValidDataId: "console_room_9x10").
    [[nodiscard]] std::string dataId(std::string_view key) const;
    /// A prefab member: {"manifest": <a prefab id>, "kind": <a prefab kind>}.
    [[nodiscard]] PrefabRef prefab(std::string_view key) const;

private:
    DocumentReader& m_document;
    const nlohmann::json* m_object = nullptr;
    std::string m_pointer;
};

/// Reads every element of an array member: `read(element, pointer)` for each, in order.
template <class Read>
void forEachElement(const nlohmann::json& array, std::string_view arrayPointer, Read&& read) {
    for (std::size_t i = 0; i < array.size(); ++i) {
        read(array[i], pointerChild(arrayPointer, i));
    }
}

/// Records a problem at the first element of `items` (read from the array at `arrayPointer`) that does not come
/// strictly after the one before it by `key` (a function giving a comparable value, std::tie of fields): "appears
/// twice" for an equal key, "out of order: <order>" otherwise.
template <class T, class Key>
void requireStrictOrder(DocumentReader& document, const std::vector<T>& items, std::string_view arrayPointer, Key&& key,
                        std::string_view order) {
    for (std::size_t i = 1; i < items.size() && !document.failed(); ++i) {
        const auto previous = key(items[i - 1]);
        const auto current = key(items[i]);
        if (previous == current) {
            document.fail(pointerChild(arrayPointer, i), "appears twice");
        } else if (!(previous < current)) {
            document.fail(pointerChild(arrayPointer, i), "out of order: " + std::string(order));
        }
    }
}

/// A JSON integer read as an int64 within [minimum, maximum], a problem at `pointer` otherwise.
[[nodiscard]] std::int64_t readInteger(DocumentReader& document, const nlohmann::json& value, std::string_view pointer,
                                       std::int64_t minimum, std::int64_t maximum);
/// A JSON string, a problem at `pointer` otherwise.
[[nodiscard]] std::string readString(DocumentReader& document, const nlohmann::json& value, std::string_view pointer);

/// "a string", "an integer", "a number with a fraction", ...: the type of `value` in problems.
[[nodiscard]] std::string describeType(const nlohmann::json& value);

} // namespace rv::world::detail
