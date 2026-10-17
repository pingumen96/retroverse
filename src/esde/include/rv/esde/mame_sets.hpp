#pragma once

// ES-DE's MAME lists (ADR-0008 §3): resources/MAME/mamebioses.xml and mamedevices.xml name the BIOS and device sets
// that ES-DE skips in the systems of the arcade and neogeo platforms (FileData::isArcadeAsset), and mamenames.xml gives
// the names their unscraped games show (MameNames::getCleanName). Read as ES-DE 3.4.1's MameNames reads them: each a
// bundled resource (bundledResource(): the ES-DE home's resources/ before the install folder's), the names first, then
// the BIOS sets, then the devices, and a list that is missing or cannot be parsed ends the reading there (so a missing
// mamenames.xml leaves no BIOS or device set either); the top-level <game> elements with their <mamename> and
// <realname>, the top-level <bios> and <device> elements, each text as written (never trimmed); a <mamename> listed
// twice takes its last <realname>. Every lookup compares exactly, with case, as ES-DE's std::find and hash map do.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/catalog/library.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/core/expected.hpp>

namespace rv::esde {

/// The bundled resources, as bundledResource() takes them.
inline constexpr std::string_view kMameNamesResource = "MAME/mamenames.xml";
inline constexpr std::string_view kMameBiosesResource = "MAME/mamebioses.xml";
inline constexpr std::string_view kMameDevicesResource = "MAME/mamedevices.xml";

/// The largest list read (ES-DE 3.4.1's mamenames.xml is 4.4 MiB, 42,375 names).
inline constexpr std::uintmax_t kMaxMameListBytes = 64ull * 1024ull * 1024ull;

/// The codes of the issues loadMameLists() reports (catalog::Issue::code), each naming the file.
/// A list is not there (in neither resources/ folder): the lists after it are not read either, as in ES-DE, so BIOS and
/// device sets of the arcade systems show as games, or their games keep their file names. Warning.
inline constexpr std::string_view kMameIssueListMissing = "mame-list-missing";
/// A list cannot be read or parsed: the lists after it are not read, as in ES-DE. Warning.
inline constexpr std::string_view kMameIssueListUnreadable = "mame-list-unreadable";

/// The three lists, ready for lookups.
class MameSets {
public:
    MameSets() = default;
    /// From the lists' entries in file order: `names` as (mamename, realname) pairs, of which the last of a mamename
    /// counts; `bioses` and `devices` as listed.
    MameSets(std::vector<std::pair<std::string, std::string>> names, std::vector<std::string> bioses,
             std::vector<std::string> devices);

    /// `stem` is a BIOS set (MameNames::isBios): listed in mamebioses.xml, compared exactly.
    [[nodiscard]] bool isBios(std::string_view stem) const noexcept;
    /// `stem` is a device set (MameNames::isDevice).
    [[nodiscard]] bool isDevice(std::string_view stem) const noexcept;
    /// What ES-DE's FileData::isArcadeAsset tests in an arcade or neogeo system: a BIOS or a device set.
    [[nodiscard]] bool isBiosOrDevice(std::string_view stem) const noexcept { return isBios(stem) || isDevice(stem); }
    /// MameNames::getRealName: the <realname> of `mameName`, or `mameName` itself when it has none or an empty one.
    [[nodiscard]] std::string realName(std::string_view mameName) const;

    [[nodiscard]] std::size_t nameCount() const noexcept { return m_names.size(); }
    [[nodiscard]] std::size_t biosCount() const noexcept { return m_bioses.size(); }
    [[nodiscard]] std::size_t deviceCount() const noexcept { return m_devices.size(); }

private:
    std::vector<std::pair<std::string, std::string>> m_names; ///< by mamename (ordinal), one per mamename
    std::vector<std::string> m_bioses;                        ///< sorted (ordinal), for lookups only
    std::vector<std::string> m_devices;                       ///< sorted (ordinal)
};

/// The lists as an ES-DE installation holds them.
struct MameLists {
    MameSets sets;
    /// The files read, in ES-DE's order (names, BIOS sets, devices); a file not read is not listed.
    std::vector<std::filesystem::path> files;
    /// kMameIssueListMissing, kMameIssueListUnreadable.
    std::vector<catalog::Issue> issues;
};

/// Reads the MAME lists of the library `profile` describes, where ES-DE finds them (bundledResource()). Never fails:
/// what is missing or unreadable is an issue, and the sets hold what was read before it.
[[nodiscard]] MameLists loadMameLists(const catalog::LibraryProfile& profile);

/// Reads the given files, as loadMameLists() reads those it finds (tests, tools); a missing path (nothing) is a list
/// that is not there.
[[nodiscard]] MameLists readMameListFiles(const std::optional<std::filesystem::path>& namesFile,
                                          const std::optional<std::filesystem::path>& biosesFile,
                                          const std::optional<std::filesystem::path>& devicesFile);

/// Whether a system's platforms (SystemDefinition::platforms, lower case) make it one whose BIOS and device sets ES-DE
/// skips and whose games it names from mamenames.xml: "arcade" or "neogeo" among them (PlatformIds ARCADE and
/// SNK_NEO_GEO), unless "ignore" is (ES-DE's PLATFORM_IGNORE, which replaces every other platform).
[[nodiscard]] bool isArcadePlatform(std::span<const std::string> platforms) noexcept;

/// ES-DE's Utils::String::removeParenthesis: each "(...)" and "[...]" removed, the first closing bracket of a kind with
/// the last opening one of that kind before it, again and again (inner groups first; a closing bracket with no opening
/// one before it ends the removals of its kind), then blanks (C isspace) trimmed at both ends.
[[nodiscard]] std::string esdeRemoveParenthesis(std::string_view text);

/// The name ES-DE gives a game of an arcade or neogeo system before its gamelist does (the FileData constructor:
/// MameNames::getCleanName of FileData::getCleanName): removeParenthesis of `stem` (the file name without its last
/// extension) looked up in mamenames.xml, then, with ES-DE's MAMENameStripExtraInfo on (its default),
/// removeParenthesis of that real name (a "zapzone" listed as "Zap Zone (World, set 1)" reads "Zap Zone"; a set
/// without a real name keeps its own). Empty when nothing is left: ES-DE skips such a file.
[[nodiscard]] std::string mameName(const MameSets& sets, std::string_view stem, bool stripExtraInfo);

} // namespace rv::esde
