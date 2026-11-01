#include <rv/app/settings_store.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/input/action.hpp>

namespace rv::app {
namespace {

using Json = nlohmann::json;

/// The value at the dotted `key` of `document`, null when it holds none.
[[nodiscard]] Json valueAt(const Json& document, std::string_view key) {
    const Json* at = &document;
    std::size_t start = 0;
    while (start <= key.size()) {
        const std::size_t dot = key.find('.', start);
        const std::string part(key.substr(start, dot == std::string_view::npos ? dot : dot - start));
        if (!at->is_object() || !at->contains(part)) {
            return nullptr;
        }
        at = &(*at)[part];
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return *at;
}

} // namespace

SettingsStore::SettingsStore(core::SettingsFile file, SettingsLayers below, std::vector<SettingRow> rows)
    : m_file(std::move(file)), m_below(std::move(below)), m_rows(std::move(rows)) {}

SettingsStore::~SettingsStore() {
    flush();
}

const SettingRow* SettingsStore::row(std::string_view id) const {
    const auto found =
        std::find_if(m_rows.begin(), m_rows.end(), [&](const SettingRow& candidate) { return candidate.id() == id; });
    return found != m_rows.end() ? &*found : nullptr;
}

const Json& SettingsStore::layer(std::string_view section) const {
    if (section == "video") {
        return m_below.video;
    }
    if (section == "world") {
        return m_below.world;
    }
    if (section == "player") {
        return m_below.player;
    }
    if (section == kBindingsSettingsSection) {
        return m_below.bindings;
    }
    return m_below.xr;
}

Json SettingsStore::below(const SettingRow& row) const {
    return valueAt(layer(row.section), row.key);
}

Json SettingsStore::value(const SettingRow& row) const {
    if (std::optional<Json> held = m_file.value(row.section, row.key); held && row.takes(*held)) {
        return *held;
    }
    return below(row);
}

bool SettingsStore::overridden(const SettingRow& row) const {
    const std::optional<Json> held = m_file.value(row.section, row.key);
    return held && row.takes(*held);
}

bool SettingsStore::set(const SettingRow& row, Json value) {
    if (!row.takes(value) || this->value(row) == value) {
        return false;
    }
    if (value == below(row)) {
        m_file.remove(row.section, row.key);
    } else {
        m_file.set(row.section, row.key, std::move(value));
    }
    m_dirty = true;
    return true;
}

bool SettingsStore::restore(const SettingRow& row) {
    const bool removed = m_file.remove(row.section, row.key);
    m_dirty = m_dirty || removed;
    return removed;
}

std::size_t SettingsStore::restoreAll() {
    std::size_t removed = 0;
    for (const SettingRow& row : m_rows) {
        removed += restore(row) ? 1 : 0;
    }
    return removed;
}

Json SettingsStore::document(std::string_view section, const std::function<bool(const Json&)>& accepts) const {
    std::vector<std::string_view> keys;
    for (const SettingRow& row : m_rows) {
        if (row.section == section) {
            keys.push_back(row.key);
        }
    }
    return core::acceptedOverlay(layer(section), m_file.section(section, keys),
                                 accepts ? accepts : [](const Json&) { return true; });
}

void SettingsStore::saveAsync() {
    if (!dirty()) {
        return;
    }
    flush();
    m_dirty = false;
    m_writeFailed = false;
    m_running = true;
    m_writer = std::thread([this, file = m_file] {
        if (Status saved = file.save(); !saved) {
            ++m_failures;
            m_writeFailed = true; // the changes stay in memory, in force, for the next save
            spdlog::error("settings.json was not written: {}", saved.error().toString());
        } else {
            spdlog::info("settings.json written");
        }
        ++m_writes;
        m_running = false;
    });
}

SettingsStore::WriteState SettingsStore::followWrite() {
    if (!idle()) {
        return WriteState::Pending;
    }
    if (m_writeFailed.load()) {
        return WriteState::Failed;
    }
    if (m_dirty) {
        saveAsync();
        return WriteState::Pending;
    }
    return WriteState::Written;
}

void SettingsStore::flush() {
    if (m_writer.joinable()) {
        m_writer.join();
    }
}

std::vector<std::string> shadowedSettings(const core::SettingsFile& file, const std::filesystem::path& userConfigDir) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kFiles = {
        {{"video", "video.json"}, {"world", "world.json"}, {"player", "player.json"}, {"xr", "xr.json"}}};
    std::vector<std::string> shadowed;
    if (const Json held = file.rawSection(kBindingsSettingsSection); !held.empty()) {
        if (const auto written = core::readJsonFile(userConfigDir / "bindings.json");
            written && written->contains("bindings")) {
            for (const std::string& key : core::shadowedKeys(held, (*written)["bindings"])) {
                shadowed.push_back(std::string(kBindingsSettingsSection) + "." + key);
            }
        }
    }
    for (const auto& [section, name] : kFiles) {
        const Json held = file.rawSection(section);
        if (held.empty()) {
            continue;
        }
        const auto written = core::readJsonFile(userConfigDir / core::pathFromUtf8(name));
        if (!written) {
            continue; // none, or left out whole: nothing hand-written counts
        }
        for (const std::string& key : core::shadowedKeys(held, *written)) {
            shadowed.push_back(std::string(section) + "." + key);
        }
    }
    return shadowed;
}

std::size_t applySettingsBindings(input::ActionMap& map, const core::SettingsFile& file) {
    std::vector<std::string> names;
    names.reserve(input::kActionCount);
    for (std::size_t index = 0; index < input::kActionCount; ++index) {
        names.emplace_back(input::actionName(static_cast<input::Action>(index)));
    }
    const std::vector<std::string_view> allowed(names.begin(), names.end());
    return applyBindingSection(map, file.section(kBindingsSettingsSection, allowed));
}

std::size_t applyBindingSection(input::ActionMap& map, const Json& section) {
    std::size_t changed = 0;
    for (const auto& [action, list] : section.items()) {
        if (!input::actionFromName(action)) {
            spdlog::warn("settings.json: {} in the bindings names no action: left out", action);
            continue;
        }
        const Json document{{"schemaVersion", input::ActionMap::kSchemaVersion}, {"bindings", {{action, list}}}};
        if (const auto replaced = map.applyOverrides(document, "settings.json")) {
            changed += *replaced;
        } else {
            spdlog::warn("settings.json: the bindings of {} are left out: {}", action, replaced.error().toString());
        }
    }
    return changed;
}

} // namespace rv::app
