#pragma once

// es_find_rules.xml (ADR-0008 §8): how ES-DE finds the program of %EMULATOR_X% and the folder of %CORE_X%, read as
// ES-DE 3.4.1's FindRules::loadFindRules reads it. Two files: <ES-DE home>/custom_systems/es_find_rules.xml first,
// then the bundled resources/systems/windows/es_find_rules.xml (bundledResource()); an emulator or core the custom file
// defines replaces the bundled one of that name (the first definition of a name wins, in a file too). A file that
// cannot be read or parsed is skipped, as ES-DE skips it. Parsing only: the launch translation (I2 step 10) evaluates
// the rules, in ES-DE's order (every winregistrypath entry, then winregistryvalue, systempath, staticpath; corepath for
// cores), with ES-DE's meaning of each entry:
//  - winregistrypath: an executable name, looked up under SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths (HKCU,
//    and HKLM only when the HKCU key does not exist), its default value;
//  - winregistryvalue: "<key path>\<value name>" under HKCU, else HKLM, optionally followed by "|<text>" appended
//    verbatim to the value;
//  - systempath: an executable name searched on the PATH (ES-DE's SearchPathW; Retroverse's search order is ADR-0008
//    §8's);
//  - staticpath: a path where '~' is ES-DE's home folder, %ESPATH% the install folder and %ROMPATH% the ROM root, a
//    '*' allowed in its last element (the first match in sorted order), optionally followed by "|<command>", which
//    replaces the program;
//  - corepath: a folder where %EMUPATH% is the found emulator's folder.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/library.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/esde/systems.hpp>

namespace rv::esde {

/// The types of find rules ES-DE reads on Windows.
enum class FindRuleType : std::uint8_t {
    WinRegistryPath,  ///< "winregistrypath" (emulators)
    WinRegistryValue, ///< "winregistryvalue" (emulators)
    SystemPath,       ///< "systempath" (emulators)
    StaticPath,       ///< "staticpath" (emulators)
    CorePath,         ///< "corepath" (cores)
};

/// The type's name as es_find_rules.xml writes it ("winregistrypath", ...).
[[nodiscard]] std::string_view toString(FindRuleType type) noexcept;
/// The type a name writes; nothing for another name (compared exactly, as ES-DE compares it).
[[nodiscard]] std::optional<FindRuleType> parseFindRuleType(std::string_view text) noexcept;

/// One <rule> element: its type and its <entry> elements, each as written (the text ES-DE reads: not trimmed).
struct FindRule {
    FindRuleType type = FindRuleType::StaticPath;
    std::vector<std::string> entries;

    bool operator==(const FindRule&) const = default;
};

/// An <emulator> or a <core>.
struct FindRuleSet {
    /// Its name attribute, as written ("RETROARCH", "OS-SHELL"): what %EMULATOR_<name>% and %CORE_<name>% name.
    std::string name;
    ConfigOrigin origin = ConfigOrigin::Bundled;
    /// Its rules of a valid type, in document order.
    std::vector<FindRule> rules;

    /// The entries of every rule of `type`, in document order: what ES-DE tries for that type.
    [[nodiscard]] std::vector<std::string> entries(FindRuleType type) const;

    bool operator==(const FindRuleSet&) const = default;
};

/// The find rules of an ES-DE installation.
struct FindRules {
    /// By name (ordinal), one per name.
    std::vector<FindRuleSet> emulators;
    /// By name (ordinal), one per name.
    std::vector<FindRuleSet> cores;
    /// The files read, in ES-DE's order (the custom one first).
    std::vector<std::filesystem::path> files;
    /// "find-rules-unreadable" (error: a file that cannot be read or parsed, or has no <ruleList>; it is skipped),
    /// "find-rules-missing" (warning: no bundled file), "find-rule-skipped" (warning: an emulator or core without a
    /// name, a rule without a type or of a type ES-DE does not read on Windows), "find-rule-replaced" (info: a custom
    /// definition replacing a bundled one), "find-rule-repeated" (warning: a name a file defines twice; the first
    /// counts).
    std::vector<catalog::Issue> issues;

    /// The emulator named `name` (exactly), or null.
    [[nodiscard]] const FindRuleSet* findEmulator(std::string_view name) const noexcept;
    /// The core named `name` (exactly), or null.
    [[nodiscard]] const FindRuleSet* findCore(std::string_view name) const noexcept;
    /// The entries of all the rules of all the emulators and cores.
    [[nodiscard]] std::size_t entryCount() const noexcept;

    bool operator==(const FindRules&) const = default;
};

/// The largest es_find_rules.xml read (ES-DE 3.4.1's is 52 KiB).
inline constexpr std::uintmax_t kMaxFindRulesBytes = 16ull * 1024ull * 1024ull;

/// Reads the find rules of the library `profile` describes: <ES-DE home>/custom_systems/es_find_rules.xml when it
/// exists, then the bundled file. Never fails: what cannot be read is an issue, as ES-DE logs it and goes on.
[[nodiscard]] FindRules loadFindRules(const catalog::LibraryProfile& profile);

/// Reads the given files, as loadFindRules() does with the files it finds (tests, tools): `customFile` first when
/// given, then `bundledFile` when given ("find-rules-missing" when not).
[[nodiscard]] FindRules loadFindRulesFiles(const std::optional<std::filesystem::path>& customFile,
                                           const std::optional<std::filesystem::path>& bundledFile);

} // namespace rv::esde
