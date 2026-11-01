#include <rv/core/settings_file.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/core/backups.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

namespace rv::core {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kSchemaVersionKey = "schemaVersion";
constexpr std::uintmax_t kMaxSettingsBytes = 16u * 1024u * 1024u;
/// A rename onto a file a reader holds open can fail while it does: tried again for about 200 ms.
constexpr int kSaveAttempts = 12;
constexpr std::chrono::milliseconds kSaveRetryWait{20};

/// A dotted key's parts.
[[nodiscard]] std::vector<std::string> partsOf(std::string_view key) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= key.size()) {
        const std::size_t dot = key.find('.', start);
        if (dot == std::string_view::npos) {
            parts.emplace_back(key.substr(start));
            break;
        }
        parts.emplace_back(key.substr(start, dot - start));
        start = dot + 1;
    }
    return parts;
}

/// The value at `parts` under `object`.
[[nodiscard]] const Json* find(const Json& object, const std::vector<std::string>& parts) {
    const Json* at = &object;
    for (const std::string& part : parts) {
        if (!at->is_object()) {
            return nullptr;
        }
        const auto next = at->find(part);
        if (next == at->end()) {
            return nullptr;
        }
        at = &*next;
    }
    return at;
}

void put(Json& object, const std::vector<std::string>& parts, Json value) {
    Json* at = &object;
    for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
        if (!at->is_object()) {
            *at = Json::object();
        }
        at = &(*at)[parts[index]];
    }
    if (!at->is_object()) {
        *at = Json::object();
    }
    (*at)[parts.back()] = std::move(value);
}

/// Removes parts[index...] under `object`, and the objects that leaves empty; true when it was there.
bool erase(Json& object, const std::vector<std::string>& parts, std::size_t index) {
    if (!object.is_object()) {
        return false;
    }
    const auto next = object.find(parts[index]);
    if (next == object.end()) {
        return false;
    }
    if (index + 1 == parts.size()) {
        object.erase(next);
        return true;
    }
    if (!erase(*next, parts, index + 1)) {
        return false;
    }
    if (next->is_object() && next->empty()) {
        object.erase(next);
    }
    return true;
}

/// The dotted keys of the leaves of `object`, under `prefix`.
void collectLeaves(const Json& object, const std::string& prefix, std::vector<std::string>& leaves) {
    for (const auto& [key, value] : object.items()) {
        const std::string dotted = prefix.empty() ? key : prefix + "." + key;
        if (value.is_object() && !value.empty()) {
            collectLeaves(value, dotted, leaves);
        } else {
            leaves.push_back(dotted);
        }
    }
}

/// Each loader of a run reads the file; an unreadable one is logged once per path and message.
void warnOnce(const std::filesystem::path& path, const Error& issue) {
    static std::mutex mutex;
    static std::set<std::string, std::less<>> logged;
    const std::string text = pathToUtf8Lossy(path) + ": " + issue.message;
    const std::lock_guard lock(mutex);
    if (logged.insert(text).second) {
        spdlog::warn("settings.json is left out: {}", issue.toString());
    }
}

} // namespace

std::filesystem::path settingsFileBeside(const std::filesystem::path& userConfigDir) {
    if (userConfigDir.empty()) {
        return {};
    }
    return userConfigDir.parent_path() / pathFromUtf8(kSettingsFileName);
}

Json mergeSettingsDocuments(Json base, const Json& overrides) {
    if (!base.is_object() || !overrides.is_object()) {
        return overrides;
    }
    for (const auto& [key, value] : overrides.items()) {
        const auto existing = base.find(key);
        if (existing != base.end() && existing->is_object() && value.is_object()) {
            *existing = mergeSettingsDocuments(std::move(*existing), value);
        } else {
            base[key] = value;
        }
    }
    return base;
}

SettingsFile SettingsFile::load(const std::filesystem::path& path) {
    SettingsFile file;
    file.m_path = path;
    const std::string context = pathToUtf8Lossy(path);
    std::error_code ignored;
    if (path.empty() || !std::filesystem::exists(path, ignored)) {
        return file;
    }
    // Full sharing: another thread's loader must never stand in the way of the rename that replaces this file.
    Expected<std::string> text = readFileShared(path, kMaxSettingsBytes);
    if (!text) {
        if (text.error().code != ErrorCode::NotFound) {
            file.m_issue = text.error();
        }
        return file;
    }
    try {
        // No comments: the file is the program's, and writing it back would lose them.
        Json parsed = Json::parse(*text, nullptr, /*allow_exceptions*/ true, /*ignore_comments*/ false);
        if (!parsed.is_object()) {
            file.m_issue = Error{ErrorCode::ParseError, "expected an object", context};
            return file;
        }
        file.m_document = std::move(parsed);
    } catch (const std::exception& error) {
        file.m_issue = Error{ErrorCode::ParseError, error.what(), context};
    }
    return file;
}

SettingsFile SettingsFile::loadBeside(const std::filesystem::path& userConfigDir) {
    SettingsFile file = load(settingsFileBeside(userConfigDir));
    if (file.m_issue) {
        warnOnce(file.m_path, *file.m_issue);
    }
    return file;
}

Status SettingsFile::backUpUnreadable() {
    if (!m_issue) {
        return ok();
    }
    auto backup = backUpFile(m_path, m_path.parent_path() / "backups");
    if (!backup) {
        return propagate(backup.error());
    }
    m_keptAside = true;
    spdlog::warn("settings.json could not be used: kept as {}, replaced at the next write",
                 pathToUtf8Lossy(backup->backup));
    return ok();
}

Json SettingsFile::section(std::string_view name, std::span<const std::string_view> allowedKeys) const {
    Json shaped = Json::object();
    const Json held = rawSection(name);
    for (const std::string_view key : allowedKeys) {
        const std::vector<std::string> parts = partsOf(key);
        if (const Json* value = find(held, parts)) {
            put(shaped, parts, *value);
        }
    }
    return shaped;
}

Json SettingsFile::rawSection(std::string_view name) const {
    const auto found = m_document.find(std::string(name));
    return found != m_document.end() && found->is_object() ? *found : Json::object();
}

std::optional<Json> SettingsFile::value(std::string_view name, std::string_view key) const {
    const Json held = rawSection(name);
    if (const Json* value = find(held, partsOf(key))) {
        return std::optional<Json>(std::in_place, *value);
    }
    return std::nullopt;
}

void SettingsFile::set(std::string_view name, std::string_view key, Json value) {
    Json& held = m_document[std::string(name)];
    put(held, partsOf(key), std::move(value));
}

bool SettingsFile::remove(std::string_view name, std::string_view key) {
    const auto found = m_document.find(std::string(name));
    if (found == m_document.end()) {
        return false;
    }
    const bool removed = erase(*found, partsOf(key), 0);
    if (removed && found->is_object() && found->empty()) {
        m_document.erase(found);
    }
    return removed;
}

std::size_t SettingsFile::removeAll(std::string_view name, std::span<const std::string_view> keys) {
    std::size_t removed = 0;
    for (const std::string_view key : keys) {
        removed += remove(name, key) ? 1 : 0;
    }
    return removed;
}

std::string SettingsFile::text() const {
    Json document = m_document;
    if (!document.contains(std::string(kSchemaVersionKey))) {
        document[std::string(kSchemaVersionKey)] = kSettingsSchemaVersion;
    }
    // Strings stay as read: load() took valid UTF-8 alone, and what set() takes comes from the program.
    return document.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
}

Status SettingsFile::save() const {
    if (m_path.empty()) {
        return makeError(ErrorCode::InvalidArgument, "settings.json has no path");
    }
    if (m_issue && !m_keptAside) {
        return makeError(ErrorCode::Conflict, "the unreadable file is not replaced: it could not be kept aside",
                         pathToUtf8Lossy(m_path));
    }
    std::error_code error;
    std::filesystem::create_directories(m_path.parent_path(), error);
    if (error) {
        return makeError(ErrorCode::IoError, error.message(), pathToUtf8Lossy(m_path.parent_path()));
    }
    const std::string content = text();
    Status written = writeTextFileAtomic(m_path, content);
    for (int attempt = 1; attempt < kSaveAttempts && !written; ++attempt) {
        std::this_thread::sleep_for(kSaveRetryWait);
        written = writeTextFileAtomic(m_path, content);
    }
    return written;
}

Json acceptedOverlay(Json base, const Json& overlay, const std::function<bool(const Json&)>& accepts,
                     std::vector<std::string>* refused) {
    if (!overlay.is_object() || overlay.empty()) {
        return base;
    }
    Json whole = mergeSettingsDocuments(base, overlay);
    if (accepts(whole)) {
        return whole;
    }
    std::vector<std::string> leaves;
    collectLeaves(overlay, {}, leaves);
    for (const std::string& leaf : leaves) {
        const std::vector<std::string> parts = partsOf(leaf);
        Json one = Json::object();
        put(one, parts, *find(overlay, parts));
        Json merged = mergeSettingsDocuments(base, one);
        if (accepts(merged)) {
            base = std::move(merged);
        } else if (refused != nullptr) {
            refused->push_back(leaf);
        }
    }
    return base;
}

std::vector<std::string> shadowedKeys(const Json& overlay, const Json& handWritten) {
    std::vector<std::string> leaves;
    collectLeaves(overlay, {}, leaves);
    std::vector<std::string> shadowed;
    for (const std::string& leaf : leaves) {
        const std::vector<std::string> parts = partsOf(leaf);
        const Json* written = find(handWritten, parts);
        if (written != nullptr && *written != *find(overlay, parts)) {
            shadowed.push_back(leaf);
        }
    }
    return shadowed;
}

} // namespace rv::core
