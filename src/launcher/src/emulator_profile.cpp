#include <rv/launcher/emulator_profile.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <spdlog/spdlog.h>

#include <rv/core/json.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

namespace rv::launcher {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kSchemaVersionField = "schemaVersion";
constexpr std::string_view kProfilesField = "profiles";

/// Where a value sits: the document's name and a JSON pointer (RFC 6901).
class Location {
public:
    explicit Location(std::string file) : m_file(std::move(file)) {}

    [[nodiscard]] Location child(std::string_view key) const {
        Location location(*this);
        location.m_pointer.push_back('/');
        for (const char c : key) {
            if (c == '~') {
                location.m_pointer += "~0";
            } else if (c == '/') {
                location.m_pointer += "~1";
            } else {
                location.m_pointer.push_back(c);
            }
        }
        return location;
    }
    [[nodiscard]] Location child(std::size_t index) const {
        Location location(*this);
        location.m_pointer += "/" + std::to_string(index);
        return location;
    }
    /// "<file>:<pointer>", or the file alone at the document's root.
    [[nodiscard]] std::string context() const { return m_pointer.empty() ? m_file : m_file + ":" + m_pointer; }

private:
    std::string m_file;
    std::string m_pointer;
};

[[nodiscard]] tl::unexpected<Error> fail(ErrorCode code, std::string message, const Location& at) {
    return makeError(code, std::move(message), at.context());
}

[[nodiscard]] std::string inQuotes(std::string_view text) {
    return fmt::format("\"{}\"", text);
}

[[nodiscard]] bool isValidProfileId(std::string_view id) noexcept {
    const auto lowerOrDigit = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (id.empty() || id.size() > kMaxProfileIdLength || !lowerOrDigit(id.front())) {
        return false;
    }
    return std::all_of(id.begin(), id.end(),
                       [&](char c) { return lowerOrDigit(c) || c == '.' || c == '_' || c == '-'; });
}

/// An ES-DE system name: one folder name (ES-DE names the system's ROM and media folders after it).
[[nodiscard]] bool isValidSystemName(std::string_view name) noexcept {
    if (name.empty() || name.size() > 255 || name == "." || name == "..") {
        return false;
    }
    for (const char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) {
            return false;
        }
    }
    return core::isValidUtf8(name);
}

/// The text before the first '{' that starts a token (an executable path's literal start).
[[nodiscard]] std::string_view literalPrefix(std::string_view text) {
    return text.substr(0, text.find('{'));
}

/// '~' alone or followed by a separator: the user's home folder.
[[nodiscard]] bool startsAtHome(std::string_view text) noexcept {
    return text == "~" || text.starts_with("~\\") || text.starts_with("~/");
}

[[nodiscard]] bool hasWildcard(std::string_view text) noexcept {
    return text.find_first_of("*?") != std::string_view::npos;
}

/// A template parses and names only allowed environment variables; in an executable path, no other token.
[[nodiscard]] Status checkTemplate(std::string_view text, const Location& at, bool executablePath = false) {
    auto parts = parseTemplateElement(text);
    if (!parts) {
        return fail(parts.error().code, parts.error().message, at);
    }
    for (const TemplatePart& part : *parts) {
        if (!part.token) {
            continue;
        }
        if (*part.token == TemplateToken::Environment) {
            if (!isAllowedEnvironmentVariable(part.text)) {
                return fail(ErrorCode::ValidationFailed,
                            fmt::format("{{env:{}}}: templates read only the environment variables that name folders "
                                        "({})",
                                        part.text, fmt::join(templateEnvironmentAllowList(), ", ")),
                            at);
            }
        } else if (executablePath) {
            return fail(ErrorCode::ValidationFailed,
                        fmt::format("an executable path holds only {{env:NAME}} tokens, not {{{}}}",
                                    toString(*part.token)),
                        at);
        }
    }
    return ok();
}

/// One of ExecutableSpec::paths as the loader accepts it.
[[nodiscard]] Status checkExecutablePath(std::string_view text, const Location& at) {
    if (text.empty()) {
        return fail(ErrorCode::ValidationFailed, "an empty executable path", at);
    }
    if (auto valid = checkTemplate(text, at, /*executablePath*/ true); !valid) {
        return valid;
    }
    const std::size_t lastSeparator = text.find_last_of("\\/");
    if (lastSeparator != std::string_view::npos && hasWildcard(text.substr(0, lastSeparator))) {
        return fail(ErrorCode::ValidationFailed, "wildcards ('*', '?') may appear only in the last element", at);
    }
    // A literal start must make the path absolute; '~' and a leading token are checked once expanded.
    const std::string_view prefix = literalPrefix(text);
    if (!startsAtHome(text) && !prefix.empty()) {
        const auto path = core::tryPathFromUtf8(prefix);
        // "C:" before a token ("C:{env:X}") is drive-relative: a root needs its separator.
        if (!path || !path->is_absolute()) {
            return fail(ErrorCode::ValidationFailed,
                        "an executable path must be absolute (or start with '~' or an {env:NAME} token)", at);
        }
    }
    return ok();
}

[[nodiscard]] Expected<std::string> readString(const Json& value, const Location& at, bool allowEmpty = false) {
    if (!value.is_string()) {
        return fail(ErrorCode::ParseError, "expected a string", at);
    }
    std::string text = value.get<std::string>();
    if (!allowEmpty && text.empty()) {
        return fail(ErrorCode::ValidationFailed, "expected a non-empty string", at);
    }
    return text;
}

[[nodiscard]] Expected<bool> readBool(const Json& value, const Location& at) {
    if (!value.is_boolean()) {
        return fail(ErrorCode::ParseError, "expected true or false", at);
    }
    return value.get<bool>();
}

/// An array of templates (each checked with checkTemplate()).
[[nodiscard]] Expected<std::vector<std::string>> readTemplates(const Json& value, const Location& at,
                                                               bool allowEmptyElements) {
    if (!value.is_array()) {
        return fail(ErrorCode::ParseError, "expected an array of strings", at);
    }
    std::vector<std::string> templates;
    for (std::size_t i = 0; i < value.size(); ++i) {
        auto text = readString(value[i], at.child(i), allowEmptyElements);
        if (!text) {
            return propagate(std::move(text.error()));
        }
        if (auto valid = checkTemplate(*text, at.child(i)); !valid) {
            return propagate(std::move(valid.error()));
        }
        templates.push_back(std::move(*text));
    }
    return templates;
}

[[nodiscard]] Expected<TemplateVariables> readVariables(const Json& value, const Location& at) {
    if (!value.is_object()) {
        return fail(ErrorCode::ParseError, "expected an object mapping variable names to templates", at);
    }
    TemplateVariables variables;
    for (const auto& item : value.items()) {
        const Location variableAt = at.child(item.key());
        if (!isValidVariableName(item.key())) {
            return fail(ErrorCode::ValidationFailed,
                        fmt::format("invalid variable name {} (1 to 64 ASCII letters, digits, '_', '-' or '.')",
                                    inQuotes(item.key())),
                        variableAt);
        }
        auto text = readString(item.value(), variableAt, /*allowEmpty*/ true);
        if (!text) {
            return propagate(std::move(text.error()));
        }
        if (auto valid = checkTemplate(*text, variableAt); !valid) {
            return propagate(std::move(valid.error()));
        }
        variables.emplace(item.key(), std::move(*text));
    }
    return variables;
}

[[nodiscard]] Expected<ExecutableSpec> readExecutable(const Json& value, const Location& at) {
    ExecutableSpec spec;
    if (value.is_string()) {
        auto text = readString(value, at);
        if (!text) {
            return propagate(std::move(text.error()));
        }
        if (auto valid = checkExecutablePath(*text, at); !valid) {
            return propagate(std::move(valid.error()));
        }
        spec.paths.push_back(std::move(*text));
        return spec;
    }
    if (!value.is_object()) {
        return fail(ErrorCode::ParseError, "expected a path, or an object with \"appPaths\" and \"paths\"", at);
    }
    for (const auto& item : value.items()) {
        const Location fieldAt = at.child(item.key());
        if (item.key() == "appPaths") {
            auto name = readString(item.value(), fieldAt);
            if (!name) {
                return propagate(std::move(name.error()));
            }
            if (name->find_first_of("\\/:*?\"<>|") != std::string::npos || !core::isValidUtf8(*name)) {
                return fail(ErrorCode::ValidationFailed,
                            "an App Paths name is an executable's file name (\"retroarch.exe\"), without a folder or "
                            "wildcards",
                            fieldAt);
            }
            spec.appPathsName = std::move(*name);
        } else if (item.key() == "paths") {
            if (!item.value().is_array()) {
                return fail(ErrorCode::ParseError, "expected an array of paths", fieldAt);
            }
            for (std::size_t i = 0; i < item.value().size(); ++i) {
                auto text = readString(item.value()[i], fieldAt.child(i));
                if (!text) {
                    return propagate(std::move(text.error()));
                }
                if (auto valid = checkExecutablePath(*text, fieldAt.child(i)); !valid) {
                    return propagate(std::move(valid.error()));
                }
                spec.paths.push_back(std::move(*text));
            }
        } else {
            return fail(ErrorCode::ParseError, "unknown field " + inQuotes(item.key()), fieldAt);
        }
    }
    if (spec.appPathsName.empty() && spec.paths.empty()) {
        return fail(ErrorCode::ValidationFailed, "the executable names neither an App Paths name nor a path", at);
    }
    return spec;
}

[[nodiscard]] Expected<std::vector<EnvironmentOverride>> readEnvironment(const Json& value, const Location& at) {
    if (!value.is_object()) {
        return fail(ErrorCode::ParseError,
                    "expected an object mapping variable names to a value (a template) or null (removed)", at);
    }
    std::vector<EnvironmentOverride> overrides;
    for (const auto& item : value.items()) {
        const Location variableAt = at.child(item.key());
        EnvironmentOverride entry{item.key(), std::nullopt};
        if (!item.value().is_null()) {
            auto text = readString(item.value(), variableAt, /*allowEmpty*/ true);
            if (!text) {
                return propagate(std::move(text.error()));
            }
            if (auto valid = checkTemplate(*text, variableAt); !valid) {
                return propagate(std::move(valid.error()));
            }
            entry.value = std::move(*text);
        }
        if (auto valid = validateOverride(entry); !valid) {
            return fail(ErrorCode::ValidationFailed, valid.error().message, variableAt);
        }
        const auto same = std::find_if(overrides.begin(), overrides.end(), [&](const EnvironmentOverride& other) {
            return sameVariableName(other.name, entry.name);
        });
        if (same != overrides.end()) {
            return fail(ErrorCode::ValidationFailed,
                        fmt::format("{} and {} name the same environment variable", inQuotes(same->name),
                                    inQuotes(entry.name)),
                        variableAt);
        }
        overrides.push_back(std::move(entry));
    }
    return overrides;
}

[[nodiscard]] Expected<std::map<std::string, SystemSettings, std::less<>>> readSystems(const Json& value,
                                                                                       const Location& at) {
    if (!value.is_object()) {
        return fail(ErrorCode::ParseError, "expected an object mapping ES-DE system names to their settings", at);
    }
    std::map<std::string, SystemSettings, std::less<>> systems;
    for (const auto& item : value.items()) {
        const Location systemAt = at.child(item.key());
        if (!isValidSystemName(item.key())) {
            return fail(ErrorCode::ValidationFailed,
                        fmt::format("invalid system name {} (an ES-DE system name, one folder name)",
                                    inQuotes(item.key())),
                        systemAt);
        }
        if (!item.value().is_object()) {
            return fail(ErrorCode::ParseError, "expected an object (with \"variables\")", systemAt);
        }
        SystemSettings settings;
        for (const auto& field : item.value().items()) {
            const Location fieldAt = systemAt.child(field.key());
            if (field.key() != "variables") {
                return fail(ErrorCode::ParseError, "unknown field " + inQuotes(field.key()), fieldAt);
            }
            auto variables = readVariables(field.value(), fieldAt);
            if (!variables) {
                return propagate(std::move(variables.error()));
            }
            settings.variables = std::move(*variables);
        }
        systems.emplace(item.key(), std::move(settings));
    }
    return systems;
}

/// One profile. `id` receives the profile's id as soon as it is known to be valid, so that later errors name it.
[[nodiscard]] Expected<EmulatorProfile> parseProfile(const Json& value, const Location& at, std::string_view file,
                                                     ProfileOrigin origin, std::string& id) {
    if (!value.is_object()) {
        return fail(ErrorCode::ParseError, "expected a profile object", at);
    }
    EmulatorProfile profile;
    profile.origin = origin;
    profile.source = std::string(file);

    const auto idField = value.find("id");
    if (idField == value.end()) {
        return fail(ErrorCode::ParseError, "missing field \"id\"", at);
    }
    auto readId = readString(*idField, at.child("id"));
    if (!readId) {
        return propagate(std::move(readId.error()));
    }
    if (!isValidProfileId(*readId)) {
        return fail(ErrorCode::ValidationFailed,
                    fmt::format("invalid profile id {}: 1 to {} lower-case ASCII letters, digits, '.', '_' or '-', "
                                "starting with a letter or a digit",
                                inQuotes(*readId), kMaxProfileIdLength),
                    at.child("id"));
    }
    profile.id = *readId;
    id = profile.id;

    bool hasExecutable = false;
    bool hasArguments = false;
    for (const auto& item : value.items()) {
        const std::string& key = item.key();
        const Json& field = item.value();
        const Location fieldAt = at.child(key);
        if (key == "id") {
            continue;
        }
        if (key == "displayName") {
            auto name = readString(field, fieldAt);
            if (!name) {
                return propagate(std::move(name.error()));
            }
            profile.displayName = std::move(*name);
        } else if (key == "executable") {
            auto executable = readExecutable(field, fieldAt);
            if (!executable) {
                return propagate(std::move(executable.error()));
            }
            profile.executable = std::move(*executable);
            hasExecutable = true;
        } else if (key == "arguments") {
            auto arguments = readTemplates(field, fieldAt, /*allowEmptyElements*/ true);
            if (!arguments) {
                return propagate(std::move(arguments.error()));
            }
            profile.arguments = std::move(*arguments);
            hasArguments = true;
        } else if (key == "workingDirectory") {
            auto directory = readString(field, fieldAt);
            if (!directory) {
                return propagate(std::move(directory.error()));
            }
            if (auto valid = checkTemplate(*directory, fieldAt); !valid) {
                return propagate(std::move(valid.error()));
            }
            profile.workingDirectory = std::move(*directory);
        } else if (key == "environment") {
            auto environment = readEnvironment(field, fieldAt);
            if (!environment) {
                return propagate(std::move(environment.error()));
            }
            profile.environment = std::move(*environment);
        } else if (key == "wait") {
            auto text = readString(field, fieldAt);
            if (!text) {
                return propagate(std::move(text.error()));
            }
            const auto strategy = parseWaitStrategy(*text);
            if (!strategy) {
                return fail(ErrorCode::ValidationFailed,
                            "unknown wait strategy " + inQuotes(*text) + " (direct, tree or confirm)", fieldAt);
            }
            profile.waitStrategy = *strategy;
        } else if (key == "windowTimeoutSeconds") {
            if (!field.is_number()) {
                return fail(ErrorCode::ParseError, "expected a number of seconds", fieldAt);
            }
            const double seconds = field.get<double>();
            if (!std::isfinite(seconds) || seconds < 0.0 ||
                seconds > static_cast<double>(kMaxWindowTimeout.count())) {
                return fail(ErrorCode::ValidationFailed,
                            fmt::format("expected 0 (no window timeout) to {} seconds", kMaxWindowTimeout.count()),
                            fieldAt);
            }
            profile.windowTimeout = std::chrono::milliseconds(std::llround(seconds * 1000.0));
        } else if (key == "hideConsoleWindow") {
            auto hide = readBool(field, fieldAt);
            if (!hide) {
                return propagate(std::move(hide.error()));
            }
            profile.hideConsoleWindow = *hide;
        } else if (key == "variables") {
            auto variables = readVariables(field, fieldAt);
            if (!variables) {
                return propagate(std::move(variables.error()));
            }
            profile.variables = std::move(*variables);
        } else if (key == "requiredFiles") {
            auto files = readTemplates(field, fieldAt, /*allowEmptyElements*/ false);
            if (!files) {
                return propagate(std::move(files.error()));
            }
            profile.requiredFiles = std::move(*files);
        } else if (key == "systems") {
            auto systems = readSystems(field, fieldAt);
            if (!systems) {
                return propagate(std::move(systems.error()));
            }
            profile.systems = std::move(*systems);
        } else if (key == "verified") {
            auto verified = readBool(field, fieldAt);
            if (!verified) {
                return propagate(std::move(verified.error()));
            }
            profile.verified = *verified;
        } else {
            return fail(ErrorCode::ParseError, "unknown field " + inQuotes(key), fieldAt);
        }
    }
    if (!hasExecutable) {
        return fail(ErrorCode::ParseError, "missing field \"executable\"", at);
    }
    if (!hasArguments) {
        return fail(ErrorCode::ParseError, "missing field \"arguments\" (an empty array for none)", at);
    }
    if (profile.displayName.empty()) {
        profile.displayName = profile.id;
    }
    return profile;
}

/// One path of ExecutableSpec::paths expanded: '~' and {env:NAME}; absolute or an error saying why not.
[[nodiscard]] Expected<std::filesystem::path> expandExecutablePath(std::string_view entry,
                                                                   const core::ISystemProbe& probe,
                                                                   const EnvironmentLookup& environment) {
    std::optional<std::filesystem::path> home;
    std::string_view rest = entry;
    if (startsAtHome(entry)) {
        home = probe.homeFolder();
        if (!home) {
            return makeError(ErrorCode::NotFound, "the user's home folder is not known");
        }
        rest = entry.substr(1);
        while (!rest.empty() && (rest.front() == '\\' || rest.front() == '/')) {
            rest.remove_prefix(1);
        }
    }
    TemplateContext context;
    context.environment = environment;
    auto expanded = expandTemplateElement(rest, context);
    if (!expanded) {
        return propagate(std::move(expanded.error()));
    }
    auto path = core::tryPathFromUtf8(*expanded);
    if (!path) {
        return propagate(std::move(path.error()));
    }
    std::filesystem::path full = home ? *home / *path : *path;
    if (!full.is_absolute()) {
        return makeError(ErrorCode::NotFound, "not an absolute path once expanded: " + core::pathToUtf8Lossy(full));
    }
    return full.lexically_normal();
}

} // namespace

std::string_view toString(ProfileOrigin origin) noexcept {
    switch (origin) {
    case ProfileOrigin::Shipped:
        return "shipped";
    case ProfileOrigin::User:
        return "user";
    }
    return "unknown";
}

std::string_view toString(LocationRule rule) noexcept {
    switch (rule) {
    case LocationRule::AppPaths:
        return "App Paths";
    case LocationRule::Path:
        return "path";
    }
    return "unknown";
}

bool EmulatorProfile::serves(std::string_view system) const {
    return systems.find(system) != systems.end();
}

const TemplateVariables* EmulatorProfile::systemVariables(std::string_view system) const {
    const auto found = systems.find(system);
    return found == systems.end() ? nullptr : &found->second.variables;
}

std::string ProfileIssue::toString() const {
    return profileId.empty() ? error.toString() : fmt::format("profile '{}': {}", profileId, error.toString());
}

ProfileDocument parseProfileDocument(const nlohmann::json& document, std::string_view file, ProfileOrigin origin) {
    ProfileDocument result;
    const Location root{std::string(file)};
    const auto issue = [&](std::string profileId, tl::unexpected<Error> error) {
        result.issues.push_back(ProfileIssue{std::string(file), std::move(profileId), std::move(error.value())});
    };
    if (!document.is_object()) {
        issue({}, fail(ErrorCode::ParseError, "expected a JSON object with \"schemaVersion\" and \"profiles\"", root));
        return result;
    }

    // The version first: it defines which fields are known, so a newer document is Unsupported, not corrupt.
    const Location versionAt = root.child(kSchemaVersionField);
    const auto version = document.find(kSchemaVersionField);
    if (version == document.end()) {
        issue({}, fail(ErrorCode::ParseError, "missing field \"schemaVersion\"", versionAt));
        return result;
    }
    if (!version->is_number_integer()) {
        issue({}, fail(ErrorCode::ParseError, "expected an integer", versionAt));
        return result;
    }
    if (*version != kEmulatorProfilesSchemaVersion) {
        issue({}, fail(ErrorCode::Unsupported,
                       fmt::format("unsupported schemaVersion {} (expected {})", version->dump(),
                                   kEmulatorProfilesSchemaVersion),
                       versionAt));
        return result;
    }
    for (const auto& item : document.items()) {
        if (item.key() != kSchemaVersionField && item.key() != kProfilesField) {
            issue({}, fail(ErrorCode::ParseError, "unknown field " + inQuotes(item.key()), root.child(item.key())));
        }
    }

    const Location profilesAt = root.child(kProfilesField);
    const auto profiles = document.find(kProfilesField);
    if (profiles == document.end()) {
        issue({}, fail(ErrorCode::ParseError, "missing field \"profiles\"", profilesAt));
        return result;
    }
    if (!profiles->is_array()) {
        issue({}, fail(ErrorCode::ParseError, "expected an array of profiles", profilesAt));
        return result;
    }
    for (std::size_t i = 0; i < profiles->size(); ++i) {
        const Location profileAt = profilesAt.child(i);
        std::string id;
        auto profile = parseProfile((*profiles)[i], profileAt, file, origin, id);
        if (!profile) {
            result.issues.push_back(ProfileIssue{std::string(file), std::move(id), std::move(profile.error())});
            continue;
        }
        const bool duplicate = std::any_of(result.profiles.begin(), result.profiles.end(),
                                           [&](const EmulatorProfile& other) { return other.id == profile->id; });
        if (duplicate) {
            issue(profile->id,
                  fail(ErrorCode::ValidationFailed,
                       "a second profile with the id " + inQuotes(profile->id) + " (the first one is kept)",
                       profileAt.child("id")));
            continue;
        }
        result.profiles.push_back(std::move(*profile));
    }
    return result;
}

ProfileDocument loadProfileDocument(const std::filesystem::path& file, ProfileOrigin origin) {
    const std::string name = core::pathToUtf8Lossy(file);
    auto document = core::readJsonFile(file);
    if (!document) {
        ProfileDocument failed;
        failed.issues.push_back(ProfileIssue{name, {}, std::move(document.error())});
        return failed;
    }
    return parseProfileDocument(*document, name, origin);
}

const EmulatorProfile* EmulatorProfiles::find(std::string_view id) const noexcept {
    const auto found = std::find_if(profiles.begin(), profiles.end(),
                                    [&](const EmulatorProfile& profile) { return profile.id == id; });
    return found == profiles.end() ? nullptr : &*found;
}

std::vector<const EmulatorProfile*> EmulatorProfiles::serving(std::string_view system) const {
    std::vector<const EmulatorProfile*> serving;
    for (const EmulatorProfile& profile : profiles) {
        if (profile.serves(system)) {
            serving.push_back(&profile);
        }
    }
    return serving;
}

EmulatorProfiles mergeProfileDocuments(ProfileDocument shipped, ProfileDocument user) {
    EmulatorProfiles merged;
    merged.profiles = std::move(shipped.profiles);
    for (EmulatorProfile& profile : user.profiles) {
        const auto replaced = std::find_if(merged.profiles.begin(), merged.profiles.end(),
                                           [&](const EmulatorProfile& other) { return other.id == profile.id; });
        if (replaced != merged.profiles.end()) {
            *replaced = std::move(profile);
        } else {
            merged.profiles.push_back(std::move(profile));
        }
    }
    merged.issues = std::move(shipped.issues);
    merged.issues.insert(merged.issues.end(), std::make_move_iterator(user.issues.begin()),
                         std::make_move_iterator(user.issues.end()));
    return merged;
}

EmulatorProfiles loadEmulatorProfiles(const std::filesystem::path& shippedFile,
                                      const std::filesystem::path& userFile) {
    ProfileDocument shipped = loadProfileDocument(shippedFile, ProfileOrigin::Shipped);
    ProfileDocument user;
    if (!userFile.empty()) {
        std::error_code ec;
        // A missing user file is the normal case; one that cannot even be checked is read for its error.
        if (std::filesystem::exists(userFile, ec) || ec) {
            user = loadProfileDocument(userFile, ProfileOrigin::User);
        }
    }
    const std::size_t userCount = user.profiles.size();
    EmulatorProfiles merged = mergeProfileDocuments(std::move(shipped), std::move(user));
    for (const ProfileIssue& issue : merged.issues) {
        spdlog::warn("Emulator profiles: {}", issue.toString());
    }
    spdlog::info("Emulator profiles: {} in effect ({} from {})", merged.profiles.size(), userCount,
                 core::pathToUtf8Lossy(userFile));
    return merged;
}

EmulatorProfiles loadEmulatorProfiles(const core::Paths& paths) {
    // asset() refuses only absolute paths and paths that leave the asset root: never this constant one.
    auto shippedFile = paths.asset(kShippedProfilesAsset);
    return loadEmulatorProfiles(shippedFile ? *shippedFile : paths.assetRoot() / "launcher" / "emulators.json",
                                paths.configDir() / core::pathFromUtf8(kUserProfilesFileName));
}

Expected<LocatedExecutable> locateExecutable(const ExecutableSpec& spec, const core::ISystemProbe& probe,
                                             const EnvironmentLookup& environment) {
    std::vector<std::string> tried;
    if (!spec.appPathsName.empty()) {
        const auto registered = core::appPath(probe, spec.appPathsName);
        if (!registered) {
            tried.push_back(fmt::format("App Paths {} is not registered", inQuotes(spec.appPathsName)));
        } else if (!registered->is_absolute()) {
            tried.push_back(fmt::format("App Paths {} names a relative path ({})", inQuotes(spec.appPathsName),
                                        core::pathToUtf8Lossy(*registered)));
        } else if (!probe.fileExists(*registered)) {
            tried.push_back(fmt::format("App Paths {} names a missing file ({})", inQuotes(spec.appPathsName),
                                        core::pathToUtf8Lossy(*registered)));
        } else {
            return LocatedExecutable{registered->lexically_normal(), LocationRule::AppPaths, spec.appPathsName};
        }
    }
    for (const std::string& entry : spec.paths) {
        auto candidate = expandExecutablePath(entry, probe, environment);
        if (!candidate) {
            tried.push_back(fmt::format("{}: {}", entry, candidate.error().message));
            continue;
        }
        const std::string shown = core::pathToUtf8Lossy(*candidate);
        const std::string name = core::pathToUtf8Lossy(candidate->filename());
        if (hasWildcard(name)) {
            const std::vector<std::filesystem::path> matches = probe.filesMatching(candidate->parent_path(), name);
            if (matches.empty()) {
                tried.push_back(fmt::format("no file matches {}", shown));
                continue;
            }
            return LocatedExecutable{matches.front().lexically_normal(), LocationRule::Path, entry};
        }
        if (!probe.fileExists(*candidate)) {
            tried.push_back(fmt::format("{} does not exist", shown));
            continue;
        }
        return LocatedExecutable{*candidate, LocationRule::Path, entry};
    }
    if (tried.empty()) {
        return makeError(ErrorCode::NotFound, "the profile names no executable");
    }
    return makeError(ErrorCode::NotFound, fmt::format("the executable was not found: {}", fmt::join(tried, "; ")));
}

} // namespace rv::launcher
