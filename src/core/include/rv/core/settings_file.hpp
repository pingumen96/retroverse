#pragma once

// <user data>/settings.json (ADR-0018 §2): what the settings screen changes, in a file of Retroverse's own, because
// <user data>/config stays the user's and nothing ever edits a commented file. Plain JSON, no comments:
//
//   {"schemaVersion": 1, "video": {...}, "world": {...}, "bindings": {...}, "player": {...}, "xr": {...}}
//
// Each section is shaped like the file it overlays and holds only the keys the user changed. The layers, weakest first:
// the shipped defaults, the user's config/<file>.json, this file's section. Each loader names the keys the screen may set
// (section()), so a stray key can never make a strict loader refuse a file; a value a loader refuses is left out alone
// (acceptedOverlay()). What the running build does not know (keys, sections, values, a later schemaVersion) stays in the
// document and is written back as read. An unreadable file is left out whole, logged, kept aside as a backup
// (backUpUnreadable(), once, by the application) and replaced at the next write.
//
// Reading through load() writes nothing, so that every loader (retroverse-cli's too) may do it. A key is named by a
// dotted path into its section ("audio.master"); a name in the file holding a dot is not addressable, and stays as read.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/expected.hpp>

namespace rv::core {

/// The file's name under the user data directory.
inline constexpr std::string_view kSettingsFileName = "settings.json";
inline constexpr int kSettingsSchemaVersion = 1;

/// settings.json beside `userConfigDir` (<user data>/config): every loader is handed that folder, and core::Paths says
/// the same (Paths::settingsFile()). Empty for an empty folder.
[[nodiscard]] std::filesystem::path settingsFileBeside(const std::filesystem::path& userConfigDir);

/// `section`'s keys over `base`: members of objects present in both merged recursively, anything else replaced.
[[nodiscard]] nlohmann::json mergeSettingsDocuments(nlohmann::json base, const nlohmann::json& overrides);

/// settings.json's content in memory. Not thread-safe; a copy can be saved on another thread.
class SettingsFile {
public:
    /// Reads `path`. A missing file is an empty document and no issue; one that cannot be read or parsed, or whose root
    /// is not an object, is an empty document with unreadable() set: left out whole. Writes nothing.
    [[nodiscard]] static SettingsFile load(const std::filesystem::path& path);
    /// settings.json beside `userConfigDir`, read as load() reads it, without an issue logged twice in a run (each
    /// loader calls it); an empty folder gives an empty document.
    [[nodiscard]] static SettingsFile loadBeside(const std::filesystem::path& userConfigDir);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
    /// The file exists and could not be used.
    [[nodiscard]] bool unreadable() const noexcept { return m_issue.has_value(); }
    [[nodiscard]] const std::optional<Error>& issue() const noexcept { return m_issue; }
    /// Copies an unreadable file to <folder of the file>/backups (core::backUpFile), so that the next write loses
    /// nothing. Nothing without one. Errors: those of core::backUpFile.
    [[nodiscard]] Status backUpUnreadable() const;

    /// The keys of `allowedKeys` (dotted) that section `name` holds, shaped like the section; an empty object when it
    /// holds none or is not an object. Keys it holds that are not allowed never come out.
    [[nodiscard]] nlohmann::json section(std::string_view name, std::span<const std::string_view> allowedKeys) const;
    /// Section `name` as read, whatever it holds (an empty object when absent).
    [[nodiscard]] nlohmann::json rawSection(std::string_view name) const;

    /// The value of `key` in section `name`, if it holds it.
    [[nodiscard]] std::optional<nlohmann::json> value(std::string_view name, std::string_view key) const;
    /// Sets a key (objects on its way made; a value that is no object on its way replaced).
    void set(std::string_view name, std::string_view key, nlohmann::json value);
    /// Removes a key, and the objects it leaves empty, the section among them: the layer below shows through. False when
    /// it was not there.
    bool remove(std::string_view name, std::string_view key);
    /// Removes `keys` of section `name`; how many were there.
    std::size_t removeAll(std::string_view name, std::span<const std::string_view> keys);

    /// The file's text: the document with schemaVersion (kSettingsSchemaVersion unless it holds another), two spaces, a
    /// final line feed.
    [[nodiscard]] std::string text() const;
    /// Writes text() through core::writeFileAtomic, the folder made when missing. Errors: those of the write.
    [[nodiscard]] Status save() const;

private:
    std::filesystem::path m_path;
    nlohmann::json m_document = nlohmann::json::object();
    std::optional<Error> m_issue;
};

/// `overlay`'s keys (an object shaped like section()'s result) merged over `base` as far as `accepts` takes them: the
/// whole first, which is every run's case, then, refused, key by key, so that one bad value costs that value alone.
/// `refused`, when given, gets the dotted keys left out.
[[nodiscard]] nlohmann::json acceptedOverlay(nlohmann::json base, const nlohmann::json& overlay,
                                             const std::function<bool(const nlohmann::json&)>& accepts,
                                             std::vector<std::string>* refused = nullptr);

/// The dotted keys of `overlay` whose value `handWritten` holds too, with another value: what a row changed earlier now
/// shadows (ADR-0018 §2: logged once at the start).
[[nodiscard]] std::vector<std::string> shadowedKeys(const nlohmann::json& overlay, const nlohmann::json& handWritten);

} // namespace rv::core
