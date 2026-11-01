#include <rv/world/world_config.hpp>

#include <array>
#include <system_error>
#include <utility>

#include <rv/core/canonical_json.hpp>
#include <rv/core/seed.hpp>
#include <rv/core/settings_file.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

#include "data_reader.hpp"

namespace rv::world {
namespace {

using detail::DocumentReader;
using detail::ObjectReader;
using Json = nlohmann::json;

constexpr std::string_view kIncludeValues = "one of covered, all";

/// The keys a preset and world.json share, read into `config` (required) or `overrides` (optional).
void readPresetValues(const ObjectReader& reader, WorldConfig& config) {
    config.decorationDensityPermille = reader.integer("decorationDensityPermille", 0, kMaxPermille);
    config.growthReservePermille = reader.integer("growthReservePermille", 0, kMaxPermille);
    config.mainPassageWidthMm = reader.integer("mainPassageWidthMm", kMinClearWidthMm, kMaxPassageWidthMm);
    config.minClearWidthMm = reader.integer("minClearWidthMm", kMinClearWidthMm, kMaxPassageWidthMm);
    const ObjectReader placement = reader.object("placement", {"include"});
    config.include = placement.enumeration<PlacementInclude>("include", parsePlacementInclude, kIncludeValues);
    config.sharedRoomMinTitles = reader.integer("sharedRoomMinTitles", 0, kMaxSharedRoomMinTitles);
}

/// Records checkWorldConfig()'s problem, if any, at its key.
void checkInDocument(DocumentReader& document, const WorldConfig& config) {
    if (document.failed()) {
        return;
    }
    if (auto status = checkWorldConfig(config); !status) {
        document.fail("/" + status.error().context, status.error().message);
    }
}

WorldConfig readCanonicalDocument(DocumentReader& document, const Json& value) {
    const ObjectReader reader(document, value, "",
                              {"decorationDensityPermille", "growthReservePermille", "lightingProfile",
                               "mainPassageWidthMm", "minClearWidthMm", "placement", "preset", "reduceMotion", "seed",
                               "sharedRoomMinTitles"});
    WorldConfig config;
    config.preset = reader.dataId("preset");
    config.lightingProfile = reader.dataId("lightingProfile");
    config.seed = reader.nullableUnsignedInteger("seed");
    readPresetValues(reader, config);
    config.reduceMotion = reader.boolean("reduceMotion");
    checkInDocument(document, config);
    return config;
}

Json documentJson(const WorldConfig& config) {
    return Json{{"preset", config.preset},
                {"seed", config.seed ? Json(*config.seed) : Json(nullptr)},
                {"decorationDensityPermille", config.decorationDensityPermille},
                {"growthReservePermille", config.growthReservePermille},
                {"mainPassageWidthMm", config.mainPassageWidthMm},
                {"minClearWidthMm", config.minClearWidthMm},
                {"placement", Json{{"include", toString(config.include)}}},
                {"sharedRoomMinTitles", config.sharedRoomMinTitles},
                {"reduceMotion", config.reduceMotion},
                {"lightingProfile", config.lightingProfile}};
}

std::filesystem::path presetPath(const std::filesystem::path& assetRoot, std::string_view id) {
    return assetRoot / "world" / "presets" / core::pathFromUtf8(std::string(id) + ".json");
}

} // namespace

std::uint64_t worldSeed(const WorldConfig& config, const catalog::LibraryId& library) {
    return config.seed ? *config.seed : core::deriveSeed("world", library.str());
}

Status checkWorldConfig(const WorldConfig& config) {
    const auto outside = [](std::string_view key, std::int64_t value, std::int64_t minimum, std::int64_t maximum) {
        return makeError(ErrorCode::InvalidArgument,
                         "expected " + std::to_string(minimum) + " to " + std::to_string(maximum) + ", found " +
                             std::to_string(value),
                         std::string(key));
    };
    if (!isValidDataId(config.preset)) {
        return makeError(ErrorCode::InvalidArgument, "not a preset id: \"" + config.preset + "\"", "preset");
    }
    if (!isValidDataId(config.lightingProfile)) {
        return makeError(ErrorCode::InvalidArgument, "not a lighting profile id: \"" + config.lightingProfile + "\"",
                         "lightingProfile");
    }
    if (config.decorationDensityPermille < 0 || config.decorationDensityPermille > kMaxPermille) {
        return outside("decorationDensityPermille", config.decorationDensityPermille, 0, kMaxPermille);
    }
    if (config.growthReservePermille < 0 || config.growthReservePermille > kMaxPermille) {
        return outside("growthReservePermille", config.growthReservePermille, 0, kMaxPermille);
    }
    if (config.mainPassageWidthMm < kMinClearWidthMm || config.mainPassageWidthMm > kMaxPassageWidthMm) {
        return outside("mainPassageWidthMm", config.mainPassageWidthMm, kMinClearWidthMm, kMaxPassageWidthMm);
    }
    if (config.sharedRoomMinTitles < 0 || config.sharedRoomMinTitles > kMaxSharedRoomMinTitles) {
        return outside("sharedRoomMinTitles", config.sharedRoomMinTitles, 0, kMaxSharedRoomMinTitles);
    }
    if (config.minClearWidthMm < kMinClearWidthMm || config.minClearWidthMm > config.mainPassageWidthMm) {
        return makeError(ErrorCode::InvalidArgument,
                         "expected " + std::to_string(kMinClearWidthMm) + " to the main passage's width (" +
                             std::to_string(config.mainPassageWidthMm) + "), found " +
                             std::to_string(config.minClearWidthMm),
                         "minClearWidthMm");
    }
    return ok();
}

Expected<WorldConfig> parsePreset(std::string_view text, std::string_view context) {
    auto value = detail::parseDataJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    DocumentReader document(text, std::string(context));
    const ObjectReader reader(document, *value, "",
                              {"decorationDensityPermille", "growthReservePermille", "id", "mainPassageWidthMm",
                               "minClearWidthMm", "name", "placement", "schemaVersion", "sharedRoomMinTitles"});
    WorldConfig config;
    (void)reader.integer("schemaVersion", 1, 1);
    config.preset = reader.dataId("id");
    // The display name (style §18's "RetroMuseumCozy"), for the reader of the file.
    if (!document.failed() && reader.string("name").empty()) {
        document.fail(reader.pointer("name"), "must not be empty");
    }
    readPresetValues(reader, config);
    checkInDocument(document, config);
    if (document.failed()) {
        return propagate(document.error());
    }
    return config;
}

Expected<WorldConfig> loadPreset(const std::filesystem::path& assetRoot, std::string_view id) {
    if (!isValidDataId(id)) {
        return makeError(ErrorCode::InvalidArgument, "not a preset id", std::string(id));
    }
    const std::filesystem::path path = presetPath(assetRoot, id);
    auto text = detail::readDataFile(path);
    if (!text) {
        return propagate(text.error());
    }
    const std::string context = core::pathToUtf8Lossy(path);
    auto config = parsePreset(*text, context);
    if (config && config->preset != id) {
        return makeError(ErrorCode::ValidationFailed,
                         "/id: the preset's id \"" + config->preset + "\" is not its file's name \"" + std::string(id) +
                             "\"",
                         context);
    }
    return config;
}

Expected<WorldConfigOverrides> parseWorldConfigOverrides(std::string_view text, std::string_view context) {
    auto value = detail::parseDataJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    DocumentReader document(text, std::string(context));
    const ObjectReader reader(document, *value, "",
                              {"decorationDensityPermille", "growthReservePermille", "lightingProfile",
                               "mainPassageWidthMm", "minClearWidthMm", "placement", "preset", "reduceMotion", "seed",
                               "sharedRoomMinTitles"});
    WorldConfigOverrides overrides;
    if (reader.has("preset")) {
        overrides.preset = reader.dataId("preset");
    }
    if (reader.has("seed")) {
        overrides.seed = reader.nullableUnsignedInteger("seed");
    }
    overrides.decorationDensityPermille = reader.optionalInteger("decorationDensityPermille", 0, kMaxPermille);
    overrides.growthReservePermille = reader.optionalInteger("growthReservePermille", 0, kMaxPermille);
    overrides.mainPassageWidthMm = reader.optionalInteger("mainPassageWidthMm", kMinClearWidthMm, kMaxPassageWidthMm);
    overrides.minClearWidthMm = reader.optionalInteger("minClearWidthMm", kMinClearWidthMm, kMaxPassageWidthMm);
    if (reader.has("placement")) {
        const ObjectReader placement = reader.object("placement", {"include"});
        if (placement.has("include")) {
            overrides.include =
                placement.enumeration<PlacementInclude>("include", parsePlacementInclude, kIncludeValues);
        }
    }
    overrides.sharedRoomMinTitles = reader.optionalInteger("sharedRoomMinTitles", 0, kMaxSharedRoomMinTitles);
    overrides.reduceMotion = reader.optionalBoolean("reduceMotion");
    if (reader.has("lightingProfile")) {
        overrides.lightingProfile = reader.dataId("lightingProfile");
    }
    if (document.failed()) {
        return propagate(document.error());
    }
    return overrides;
}

Expected<WorldConfig> applyOverrides(WorldConfig base, const WorldConfigOverrides& overrides) {
    if (overrides.preset) {
        base.preset = *overrides.preset;
    }
    if (overrides.seed) {
        base.seed = *overrides.seed;
    }
    if (overrides.decorationDensityPermille) {
        base.decorationDensityPermille = *overrides.decorationDensityPermille;
    }
    if (overrides.growthReservePermille) {
        base.growthReservePermille = *overrides.growthReservePermille;
    }
    if (overrides.mainPassageWidthMm) {
        base.mainPassageWidthMm = *overrides.mainPassageWidthMm;
    }
    if (overrides.minClearWidthMm) {
        base.minClearWidthMm = *overrides.minClearWidthMm;
    }
    if (overrides.include) {
        base.include = *overrides.include;
    }
    if (overrides.sharedRoomMinTitles) {
        base.sharedRoomMinTitles = *overrides.sharedRoomMinTitles;
    }
    if (overrides.reduceMotion) {
        base.reduceMotion = *overrides.reduceMotion;
    }
    if (overrides.lightingProfile) {
        base.lightingProfile = *overrides.lightingProfile;
    }
    if (auto status = checkWorldConfig(base); !status) {
        return propagate(status.error());
    }
    return base;
}

namespace {

/// The default preset, or the one world.json names, with world.json's keys: the layers under settings.json.
Expected<LoadedWorldConfig> loadHandWrittenConfig(const std::filesystem::path& assetRoot,
                                                  const std::filesystem::path& userConfigDir) {
    auto defaults = loadPreset(assetRoot, kDefaultPreset);
    if (!defaults) {
        return propagate(defaults.error());
    }
    LoadedWorldConfig loaded;
    loaded.config = *defaults;

    const std::filesystem::path userPath = userConfigDir / "world.json";
    const std::string userContext = core::pathToUtf8Lossy(userPath);
    auto text = detail::readDataFile(userPath);
    if (!text) {
        if (text.error().code != ErrorCode::NotFound) {
            loaded.issues.push_back(text.error());
        }
        return loaded;
    }
    auto overrides = parseWorldConfigOverrides(*text, userContext);
    if (!overrides) {
        loaded.issues.push_back(overrides.error());
        return loaded;
    }
    WorldConfig base = *defaults;
    if (overrides->preset && *overrides->preset != kDefaultPreset) {
        auto preset = loadPreset(assetRoot, *overrides->preset);
        if (!preset) {
            Error issue = preset.error();
            issue.message =
                "the preset \"" + *overrides->preset + "\" that world.json names does not load: " + issue.message;
            loaded.issues.push_back(std::move(issue));
            return loaded;
        }
        base = *preset;
    }
    auto config = applyOverrides(base, *overrides);
    if (!config) {
        Error issue = config.error();
        issue.message = issue.context + ": " + issue.message;
        issue.context = userContext;
        loaded.issues.push_back(std::move(issue));
        return loaded;
    }
    loaded.config = *config;
    return loaded;
}

/// The keys of `overlay` a section of settings.json may set over a config: only how the world is shown.
void applyShownKeys(WorldConfig& config, const nlohmann::json& overlay, const std::filesystem::path& assetRoot,
                    std::vector<std::string>& refused) {
    const auto knownProfile = [&](const std::string& id) {
        std::error_code ignored;
        return isValidDataId(id) &&
               std::filesystem::exists(assetRoot / "world" / "lighting" / core::pathFromUtf8(id + ".json"), ignored);
    };
    for (const auto& [key, value] : overlay.items()) {
        if (key == "reduceMotion" && value.is_boolean()) {
            config.reduceMotion = value.get<bool>();
        } else if (key == "lightingProfile" && value.is_string() && knownProfile(value.get<std::string>())) {
            config.lightingProfile = value.get<std::string>();
        } else {
            refused.push_back(key);
        }
    }
}

} // namespace

std::span<const std::string_view> worldSettingKeys() noexcept {
    static constexpr std::array<std::string_view, 2> kKeys = {"reduceMotion", "lightingProfile"};
    return kKeys;
}

Expected<LoadedWorldConfig> loadWorldConfig(const std::filesystem::path& assetRoot,
                                            const std::filesystem::path& userConfigDir) {
    auto loaded = loadHandWrittenConfig(assetRoot, userConfigDir);
    if (!loaded) {
        return loaded;
    }
    loaded->belowSettings = nlohmann::json{{"reduceMotion", loaded->config.reduceMotion},
                                           {"lightingProfile", loaded->config.lightingProfile}};
    applyShownKeys(loaded->config,
                   core::SettingsFile::loadBeside(userConfigDir).section(kWorldSettingsSection, worldSettingKeys()),
                   assetRoot, loaded->refusedKeys);
    return loaded;
}

Expected<nlohmann::json> worldConfigToJson(const WorldConfig& config) {
    if (auto status = checkWorldConfig(config); !status) {
        return propagate(status.error());
    }
    return documentJson(config);
}

Expected<std::string> writeWorldConfig(const WorldConfig& config) {
    auto document = worldConfigToJson(config);
    if (!document) {
        return propagate(document.error());
    }
    return core::writeCanonicalJson(*document);
}

Expected<WorldConfig> readWorldConfig(std::string_view text, std::string_view context) {
    auto value = core::parseCanonicalJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    DocumentReader document(text, std::string(context));
    WorldConfig config = readCanonicalDocument(document, *value);
    if (document.failed()) {
        return propagate(document.error());
    }
    return config;
}

} // namespace rv::world
