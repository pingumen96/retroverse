#pragma once

// The catalog's identifiers (ADR-0008 §4): GameId, the identity of a game, assigned once and persisted; LibraryId, the
// fixed slug of a library; SystemName, an ES-DE system name as es_systems.xml writes it. Text is validated when a value
// is made from it, so that a value of these types is always well-formed (the default-constructed GameId and SystemName
// stand for "none").

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <rv/core/expected.hpp>

namespace rv::catalog {

/// The identity of a game (ADR-0008 §4): a positive 63-bit integer, derived from the game's lookup key the first time
/// the key is seen (identity.hpp) and persisted from then on as the INTEGER PRIMARY KEY of its rows. Text form: 'g' and
/// 16 lowercase hexadecimal digits ("g0123456789abcdef"), which is also a valid UI entry id ([A-Za-z0-9_-]{1,64}). 0 is
/// never a GameId: a default-constructed GameId holds it and stands for "no game".
class GameId {
public:
    /// The largest value, 2^63 - 1: SQLite's integers are signed 64-bit.
    static constexpr std::uint64_t kMaxValue = 0x7FFF'FFFF'FFFF'FFFFull;
    /// The length of the text form.
    static constexpr std::size_t kTextLength = 17;

    constexpr GameId() noexcept = default;

    /// The GameId of `value`; nothing for 0 and for values above kMaxValue.
    [[nodiscard]] static constexpr std::optional<GameId> fromValue(std::uint64_t value) noexcept {
        if (value == 0 || value > kMaxValue) {
            return std::nullopt;
        }
        return GameId(value);
    }
    /// Reads the text form back: exactly 'g' and 16 lowercase hexadecimal digits, for a value from 1 to kMaxValue.
    /// Errors: InvalidArgument naming the text.
    [[nodiscard]] static Expected<GameId> parse(std::string_view text);

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return m_value; }
    /// False for the default-constructed "no game".
    [[nodiscard]] constexpr bool isValid() const noexcept { return m_value != 0; }
    /// 'g' and 16 lowercase hexadecimal digits ("g0000000000000000" for "no game", which parse() refuses).
    [[nodiscard]] std::string toString() const;

    constexpr auto operator<=>(const GameId&) const noexcept = default;

private:
    constexpr explicit GameId(std::uint64_t value) noexcept : m_value(value) {}

    std::uint64_t m_value = 0;
};

/// The fixed slug of a library (ADR-0008 §4): chosen once (default "esde"), stored in the database, which is the
/// authority, and named by library.json. It takes part in every GameId of the library, so it never changes once games
/// have ids. 1 to 32 characters among lowercase ASCII letters, digits and '-'.
class LibraryId {
public:
    static constexpr std::size_t kMaxLength = 32;
    static constexpr std::string_view kDefault = "esde";

    /// The default library, "esde".
    LibraryId() : m_text(kDefault) {}

    /// Errors: InvalidArgument naming the text.
    [[nodiscard]] static Expected<LibraryId> parse(std::string_view text);
    [[nodiscard]] static bool isValid(std::string_view text) noexcept;

    [[nodiscard]] const std::string& str() const noexcept { return m_text; }

    auto operator<=>(const LibraryId&) const = default;

private:
    explicit LibraryId(std::string text) : m_text(std::move(text)) {}

    std::string m_text;
};

/// An ES-DE system name as es_systems.xml writes it in <name> ("gb", "trs-80"): the name of the system's folders
/// (gamelists/<name>, <media>/<name>) and part of every GameId, compared byte for byte (ES-DE's names are lowercase; a
/// folder on disk may differ in case, ES-DE builds its paths from the name). Validated as one folder name on Windows:
/// well-formed UTF-8 of 1 to 255 bytes, no character below U+0020 (DEL is allowed, as in lookup keys), none of
/// < > : " / \ | ? *, not "." or "..", no leading or trailing space and no trailing dot (which Windows would drop). A
/// default-constructed SystemName is empty and stands for "no system".
class SystemName {
public:
    static constexpr std::size_t kMaxLength = 255;

    SystemName() = default;

    /// Errors: InvalidArgument naming the text.
    [[nodiscard]] static Expected<SystemName> parse(std::string_view text);
    [[nodiscard]] static bool isValid(std::string_view text) noexcept;

    [[nodiscard]] const std::string& str() const noexcept { return m_text; }
    [[nodiscard]] bool empty() const noexcept { return m_text.empty(); }

    auto operator<=>(const SystemName&) const = default;

private:
    explicit SystemName(std::string text) : m_text(std::move(text)) {}

    std::string m_text;
};

namespace detail {

/// The value of an enumeration of the catalog whose toString() is `text`; its values run from 0 to `last`. Gives every
/// enumeration with names its parse function.
template <class Enum>
[[nodiscard]] constexpr std::optional<Enum> enumFromString(std::string_view text, Enum last) noexcept {
    for (int value = 0; value <= static_cast<int>(last); ++value) {
        if (toString(static_cast<Enum>(value)) == text) {
            return static_cast<Enum>(value);
        }
    }
    return std::nullopt;
}

} // namespace detail

} // namespace rv::catalog

/// GameIds as keys of unordered containers (never iterated into an output: CLAUDE.md, determinism).
template <>
struct std::hash<rv::catalog::GameId> {
    [[nodiscard]] std::size_t operator()(rv::catalog::GameId id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value());
    }
};
