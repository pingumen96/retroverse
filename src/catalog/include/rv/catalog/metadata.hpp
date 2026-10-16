#pragma once

// Game metadata as ES-DE's gamelists hold it, and the normalizers of its values (ADR-0008 §5, specification §4.3:
// normalized internally without changing the original values). Text fields are kept exactly as written (decoded,
// trailing spaces and line feeds included) and cleaned only where they are shown on one line (displayText()); dates,
// ratings, players, booleans and counts are parsed into values, and the text they were parsed from is kept beside them
// (RawEsdeValues), whether it parsed or not. Formats as ES-DE 3.4 writes them (the survey of the reference library):
// date-times "19970702T000000", ratings "0.1" to "0.9" and "1", players "1", "1-2", "2-16", "8+", "24", "2-3000".

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>

namespace rv::catalog {

/// A date and time as ES-DE writes them in gamelists, "YYYYMMDDTHHMMSS" (ISO 8601 basic format), without a time zone:
/// ES-DE's <lastplayed> is the local time of the machine when the game ended; release dates carry T000000.
struct LocalDateTime {
    int year = 1;   ///< 1 to 9999
    int month = 1;  ///< 1 to 12
    int day = 1;    ///< 1 to the length of the month (leap years counted)
    int hour = 0;   ///< 0 to 23
    int minute = 0; ///< 0 to 59
    int second = 0; ///< 0 to 59

    auto operator<=>(const LocalDateTime&) const = default;
};

/// Reads "YYYYMMDDTHHMMSS" (ASCII white space around it ignored). Errors: ParseError for any other form or a date or
/// time that does not exist ("19990230T000000").
[[nodiscard]] Expected<LocalDateTime> parseEsdeDateTime(std::string_view text);
/// "YYYYMMDDTHHMMSS", as ES-DE writes it.
[[nodiscard]] std::string formatEsdeDateTime(const LocalDateTime& time);

/// A release date: the date of ES-DE's <releasedate>, its time (always T000000) dropped. Many dates are 1 January, a
/// year-only date as scrapers write it ("19900101T000000"): that cannot be told from a real 1 January.
struct ReleaseDate {
    int year = 1;
    int month = 1;
    int day = 1;

    auto operator<=>(const ReleaseDate&) const = default;
};

/// parseEsdeDateTime() reduced to the date.
[[nodiscard]] Expected<ReleaseDate> parseReleaseDate(std::string_view text);

/// A rating in tenths, 0 to 10: ES-DE's <rating>, a fraction from 0 to 1 that its theme shows as five stars in half
/// steps ("0.7" is 7: three and a half stars).
struct Rating {
    int tenths = 0;

    auto operator<=>(const Rating&) const = default;
};

/// Reads "0", "1", or "0." or "1." and decimal digits, a value from 0 to 1 (ASCII white space around it ignored);
/// ES-DE writes one decimal, other tools sometimes two ("0.75"), rounded to the nearest tenth, halves up. Errors:
/// ParseError for anything else (a sign, an exponent, a value above 1).
[[nodiscard]] Expected<Rating> parseRating(std::string_view text);
/// "0", "0.1" to "0.9", "1": ES-DE's form.
[[nodiscard]] std::string formatRating(Rating rating);

/// The number of players: ES-DE's <players>, free text in which scrapers write "1", "1-2", "2-16", "24", "8+" or
/// "2-3000".
struct Players {
    std::uint32_t min = 1;
    /// Nothing for an open range ("8+": eight or more).
    std::optional<std::uint32_t> max = 1;

    auto operator<=>(const Players&) const = default;
};

/// Reads "N", "N-M" (N <= M) and "N+", decimal numbers from 1 to 4294967295 (ASCII white space around it ignored).
/// Errors: ParseError for anything else ("0", "1, 2", "unknown").
[[nodiscard]] Expected<Players> parsePlayers(std::string_view text);
/// "N", "N-M" or "N+".
[[nodiscard]] std::string formatPlayers(const Players& players);

/// ES-DE's booleans (<favorite>, <hidden>, <kidgame>, <completed>, <broken>): exactly "true" or "false". ES-DE takes
/// anything but "true" as false; the callers do the same with what this refuses. Errors: ParseError.
[[nodiscard]] Expected<bool> parseEsdeBool(std::string_view text);

/// ES-DE's counts (<playcount>, <playtime> in seconds): decimal digits for 0 to 2^63 - 1, what the database can hold
/// (ASCII white space around it ignored). Errors: ParseError.
[[nodiscard]] Expected<std::uint64_t> parseEsdeCount(std::string_view text);

/// A text field as shown on one line (titles, developers, publishers, genres): ASCII white space removed at both ends,
/// and every run of white space inside that holds a line break or a tab replaced by one space ("Mirage Soft \n" reads
/// "Mirage Soft"); runs of spaces alone are kept as written ("10 out of 10 :  Volcanoes").
[[nodiscard]] std::string displayText(std::string_view raw);

/// The title of a file without metadata (an unscraped game): its file name's stem as ES-DE shows it, which is the name
/// without its last extension ("Game (Europe).dec.iso" reads "Game (Europe).dec"), the name of a folder kept whole;
/// then ASCII white space removed at both ends ("Disk .atr" reads "Disk"). Nothing else changes: parentheses, brackets,
/// tags and leading dots stay ("Game (USA) [!]"). A stem that is empty after that (a file named ".gb") gives the whole
/// name, trimmed. `relativePath` is a LookupKey::relativePath(); its last element is the name.
[[nodiscard]] std::string titleFromFileStem(std::string_view relativePath, bool directory = false);

/// The version of the sort key folding of sortKeyOf(), stored with the sort keys: a new folding means new keys.
inline constexpr std::uint32_t kSortKeyVersion = 1;

/// The sort key of a title (version 1), by which the catalog orders its lists (ADR-0008 §5: the total order is the sort
/// key, then the GameId): the title's ASCII white space removed at both ends and every run of it inside made one space,
/// then lower-cased with the invariant locale (core::invariantLowerCase, code unit by code unit). Compared ordinally,
/// by UTF-8 bytes. Deliberately simple: leading articles are not moved ("The Ninja" sorts under T), digits compare as
/// text ("10" before "2"), accented letters are not folded (they sort by code point, after "z"). Errors: malformed
/// UTF-8 (InvalidArgument), a failure of the system call (PlatformError).
[[nodiscard]] Expected<std::string> sortKeyOf(std::string_view title);

/// The metadata of an entry, from its gamelist entry (ES-DE's <game> or <folder>); all empty for a file that has none.
/// Text fields hold the element's text exactly as written (decoded; trailing spaces and line feeds kept): displayText()
/// is for showing them on one line. Normalized fields hold the value, or nothing when the element is missing or its
/// text does not parse; the text as written is in RawEsdeValues::originalForms either way.
struct GameMetadata {
    std::string name;        ///< <name>
    std::string sortName;    ///< <sortname>: what the entry sorts by instead of its title, when not blank
    std::string description; ///< <desc>
    std::string developer;   ///< <developer>
    std::string publisher;   ///< <publisher>
    std::string genre;       ///< <genre> (a single text: "Corsa, Guida / Moto" is one genre)
    std::optional<ReleaseDate> releaseDate; ///< <releasedate>
    std::optional<Rating> rating;           ///< <rating>
    std::optional<Players> players;         ///< <players>

    bool operator==(const GameMetadata&) const = default;
};

/// The title an entry shows when the adapter has nothing better (an arcade name, say): its <name> as displayText()
/// shows it, else titleFromFileStem() of its relative path.
[[nodiscard]] std::string defaultTitle(const GameMetadata& metadata, std::string_view relativePath,
                                       bool directory = false);

/// An element of a gamelist entry kept as ES-DE wrote it (ADR-0008 §5).
struct RawElement {
    std::string name; ///< the element's name ("releasedate", "region")
    std::string text; ///< its character data, decoded, exactly as written

    auto operator<=>(const RawElement&) const = default;
};

/// What an entry keeps of its gamelist entry besides the modelled values (ADR-0008 §5: the raw ES-DE values kept are
/// the unknown elements and the original forms of normalized fields), so that nothing ES-DE wrote is lost or altered.
struct RawEsdeValues {
    /// The elements Retroverse does not model (isModelledGamelistElement() false: ES-DE's <controller> or
    /// <collectionsortname>, other tools' <region> or <md5>), in document order, repeated ones each kept. Their
    /// attributes and child elements are not kept.
    std::vector<RawElement> unknownElements;
    /// The text of each normalized element present (isNormalizedGamelistElement()), as written, in document order: also
    /// when it parsed, so that "0.70" or " 3" stay what they were.
    std::vector<RawElement> originalForms;

    /// The original form of the normalized element `name`, or nullptr.
    [[nodiscard]] const std::string* originalForm(std::string_view name) const noexcept {
        for (const RawElement& element : originalForms) {
            if (element.name == name) {
                return &element.text;
            }
        }
        return nullptr;
    }
    [[nodiscard]] bool empty() const noexcept { return unknownElements.empty() && originalForms.empty(); }

    bool operator==(const RawEsdeValues&) const = default;
};

/// True for the elements of ES-DE's <game> and <folder> entries that the catalog models: <path>, the text fields of
/// GameMetadata (<name>, <sortname>, <desc>, <developer>, <publisher>, <genre>), <altemulator>, and the normalized
/// elements. Every other element is kept as an unknown element.
[[nodiscard]] bool isModelledGamelistElement(std::string_view name) noexcept;

/// True for the modelled elements whose text is parsed into a value, and therefore also kept as written: <releasedate>,
/// <rating>, <players>, <favorite>, <hidden>, <kidgame>, <completed>, <broken>, <playcount>, <playtime>, <lastplayed>.
[[nodiscard]] bool isNormalizedGamelistElement(std::string_view name) noexcept;

} // namespace rv::catalog
