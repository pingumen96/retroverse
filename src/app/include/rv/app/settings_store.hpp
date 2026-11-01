#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.
//
// The settings of a run as the screen sees them (ADR-0018 §2, §3): the rows of settingRows() over settings.json in memory
// and the documents under it. A change goes to memory at once; the file is written off the main thread when a screen
// closes and before a launch (saveAsync()). A value that equals the layer below is no change: its key goes, so that the
// file holds only what the user changed and a hand-written file shows through again.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/app/setting_rows.hpp>
#include <rv/core/settings_file.hpp>
#include <rv/input/action_map.hpp>

namespace rv::app {

/// The documents under settings.json, section by section (the loaders' belowSettings).
struct SettingsLayers {
    nlohmann::json video = nlohmann::json::object();
    nlohmann::json world = nlohmann::json::object();
    nlohmann::json player = nlohmann::json::object();
    nlohmann::json xr = nlohmann::json::object();
    /// Action names over binding lists (ActionMap::toJson()'s "bindings"), the user's file over the shipped map.
    nlohmann::json bindings = nlohmann::json::object();
};

class SettingsStore {
public:
    SettingsStore(core::SettingsFile file, SettingsLayers below, std::vector<SettingRow> rows);
    ~SettingsStore();
    SettingsStore(const SettingsStore&) = delete;
    SettingsStore& operator=(const SettingsStore&) = delete;

    [[nodiscard]] const std::vector<SettingRow>& rows() const noexcept { return m_rows; }
    /// The row of `id` (SettingRow::id()); null when there is none.
    [[nodiscard]] const SettingRow* row(std::string_view id) const;

    /// The value under settings.json.
    [[nodiscard]] nlohmann::json below(const SettingRow& row) const;
    /// The value the row shows: settings.json's when the row can hold it, else the one below.
    [[nodiscard]] nlohmann::json value(const SettingRow& row) const;
    /// settings.json holds a value the row takes ("Ripristina" shows).
    [[nodiscard]] bool overridden(const SettingRow& row) const;

    /// Sets the row's value (refused, false, when the row cannot hold it or it shows already).
    bool set(const SettingRow& row, nlohmann::json value);
    /// "Ripristina": removes the row's key; false when it held none.
    bool restore(const SettingRow& row);
    /// "Ripristina tutto": removes the key of every row; how many. Keys no row names stay.
    std::size_t restoreAll();
    /// `section`'s document under settings.json with the keys of the rows over it as far as `accepts` (the loader's
    /// parser) takes them: what a loader would give now, a value a row cannot show but the loader takes included.
    [[nodiscard]] nlohmann::json document(std::string_view section,
                                          const std::function<bool(const nlohmann::json&)>& accepts = {}) const;

    /// Changes wait to be written, those of a write that failed among them.
    [[nodiscard]] bool dirty() const noexcept { return m_dirty || m_writeFailed.load(); }
    /// Writes the file on a thread of its own when changes wait (the previous write waited for first): a write that
    /// failed is tried again here.
    void saveAsync();
    /// Where the changes stand, for what reads the file (the floor's job): a write runs, or was started now for what
    /// changed since (Pending); the file holds them (Written); the last write failed (Failed: the next saveAsync() tries
    /// again, followWrite() does not).
    enum class WriteState : std::uint8_t { Pending, Written, Failed };
    [[nodiscard]] WriteState followWrite();
    /// Waits for the write under way.
    void flush();
    /// No write is under way.
    [[nodiscard]] bool idle() const noexcept { return !m_running.load(); }
    /// Writes finished, and those that failed (logged).
    [[nodiscard]] std::uint64_t writes() const noexcept { return m_writes.load(); }
    [[nodiscard]] std::uint64_t writeFailures() const noexcept { return m_failures.load(); }

private:
    [[nodiscard]] const nlohmann::json& layer(std::string_view section) const;

    core::SettingsFile m_file;
    SettingsLayers m_below;
    std::vector<SettingRow> m_rows;
    bool m_dirty = false;
    std::thread m_writer;
    std::atomic<bool> m_running{false};
    std::atomic<std::uint64_t> m_writes{0};
    std::atomic<std::uint64_t> m_failures{0};
    std::atomic<bool> m_writeFailed{false};
};

/// "<section>.<key>" of every key of `file` that shadows a hand-written value of <userConfigDir>/<section's file>
/// (ADR-0018 §2: logged once at the start). Reads only.
[[nodiscard]] std::vector<std::string> shadowedSettings(const core::SettingsFile& file,
                                                        const std::filesystem::path& userConfigDir);

/// settings.json's bindings over `map` (ADR-0018 §7: each action named replaces that action's, after the user's file), a
/// list applyOverrides() refuses left out alone (logged). How many actions changed.
std::size_t applySettingsBindings(input::ActionMap& map, const core::SettingsFile& file);

} // namespace rv::app
