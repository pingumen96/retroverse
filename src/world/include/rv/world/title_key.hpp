#pragma once

// Title keys and version tags (ADR-0010 §3, I3 step 2): what groups a system's games into titles, one cartridge each,
// and what chooses each title's representative among its versions.
//
// The title key (rule kTitleKeyRule, "title-key/1") of a display title (RoomEntry::title: the gamelist's <name>, else
// the catalog's title of the file name):
//  1. every group in parentheses or brackets removed: a '(' up to the ')' that balances it, the parentheses between
//     them counted, and alike a '[' up to its ']'; whatever the group holds goes with it (brackets in parentheses too).
//     The text is scanned from the left: a group starts at the first opening character not inside an earlier group. An
//     opening character that nothing balances, and a closing one outside a group, stay as text;
//  2. ASCII white space trimmed at both ends and every run of it inside made one space;
//  3. the subtitle separators " : ", ": ", " - " and " – " (an en dash, U+2013, between spaces) made one, ": ", read
//     from the left ("Quest - Part : Two" reads "Quest: Part: Two");
//  4. folded by the catalog's sort key (catalog::sortKeyOf, version catalog::kSortKeyVersion: white space again, then
//     the invariant lower case), recorded with the selection.
// A title made only of groups ("(Untitled)") would have an empty key: its key is the title folded by step 4 alone.
// Nothing else is folded: accents, digits and punctuation other than the separators count ("Quest II" and "Quest 2"
// are two titles), as the catalog's sort keys do.
//
// The version tags are read from the file's name (the last element of LookupKey::relativePath(), extension included),
// never from the title, since a gamelist's name drops them:
//  - the region tag: the first group in parentheses that is made only of No-Intro region names separated by ", "
//    ("(USA)", "(Japan, USA)", "(USA, Europe)") or is a GoodTools country code ("(U)", "(E)", "(JU)", "(UE)", "(4)");
//    its rank is 0 when it names USA or World (the codes U and W, alone or combined, and 4, USA and Brazil), 1 when it
//    names Europe (E, alone or combined) and not USA or World, 2 otherwise and without a region tag. Only these exact
//    spellings count: "(UK)" is England, "(Unl)", "(PD)" and "(Unk)" are no country;
//  - a status tag: a group in parentheses one of whose items (", " between them) starts with the word Beta, Proto,
//    Demo, Sample or Pirate ("(Beta)", "(Proto 2)", "(Demo)"), or a GoodTools mark in brackets "[h...]", "[b...]" or
//    "[a...]" (a hack, a bad dump, an alternative: the letter alone or followed by anything but a lower-case letter,
//    "[h1C]", "[b1]", "[a]");
//  - the revision: the highest N of the items "Rev N" (decimal digits) of the groups in parentheses ("(Rev 1)",
//    "(USA) (Rev 2)"); none counts as 0, and so does a revision written otherwise ("(Rev A)", GoodTools' "(V1.1)").
// The representative of a title is the first of its versions by: region rank, without a status tag before with one,
// the highest revision (versionTagsLess()), then the lookup key and the GameId (selectRoom(), room_selection.hpp).
//
// scripts/title_key_oracle.py computes the same rules independently (Python) for the table of
// tests/unit/world/test_title_key.cpp, which holds its output.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <rv/core/expected.hpp>

namespace rv::world {

/// The rule's name, recorded in RoomSelection::titleKeyRule and the plan.
inline constexpr std::string_view kTitleKeyRule = "title-key/1";

/// The title key of a display title (see above). Errors: those of catalog::sortKeyOf (malformed UTF-8).
[[nodiscard]] Expected<std::string> titleKey(std::string_view title);

/// The rank of a version's region tag: lower is preferred.
enum class RegionRank : std::uint8_t {
    UsaOrWorld = 0, ///< the region tag names USA or World
    Europe = 1,     ///< it names Europe, and neither USA nor World
    Other = 2,      ///< any other region, or no region tag
};

/// "usa-or-world", "europe", "other".
[[nodiscard]] constexpr std::string_view toString(RegionRank rank) noexcept {
    switch (rank) {
    case RegionRank::UsaOrWorld:
        return "usa-or-world";
    case RegionRank::Europe:
        return "europe";
    case RegionRank::Other:
        return "other";
    }
    return "unknown";
}

/// What a file's name says of its version.
struct VersionTags {
    RegionRank region = RegionRank::Other;
    /// The region tag's text as written ("USA, Europe", "JU"); nothing without one.
    std::optional<std::string> regionTag;
    /// A status tag: a beta, prototype, demo, sample or pirate, a hack, bad dump or alternative dump.
    bool status = false;
    /// The highest "Rev N" (0 without one).
    std::uint32_t revision = 0;

    bool operator==(const VersionTags&) const = default;
};

/// The tags of `fileName`, a file's or folder's name (see above).
[[nodiscard]] VersionTags readVersionTags(std::string_view fileName);

/// The last element of a relative path ('/' between the elements): what readVersionTags() reads.
[[nodiscard]] std::string_view fileNameOf(std::string_view relativePath) noexcept;

/// The order of the versions of one title, by their tags only: region rank, without a status tag first, the highest
/// revision first. Callers break ties with the lookup key and the GameId.
[[nodiscard]] bool versionTagsLess(const VersionTags& a, const VersionTags& b) noexcept;

} // namespace rv::world
