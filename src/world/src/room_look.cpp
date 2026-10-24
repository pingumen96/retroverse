#include <rv/world/room_look.hpp>

#include <algorithm>
#include <system_error>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/platform_manifest.hpp>
#include <rv/world/world_config.hpp>

#include "data_reader.hpp"

namespace rv::world {
namespace {

using detail::DocumentReader;
using detail::ObjectReader;
using Json = nlohmann::json;

constexpr std::string_view kColourForm = "an sRGB colour #RRGGBB in uppercase hexadecimal";
constexpr double kMaxIntensity = 1000.0;
constexpr double kMaxRange = 100.0;

std::filesystem::path lightingPath(const std::filesystem::path& assetRoot, std::string_view id) {
    return assetRoot / "world" / "lighting" / core::pathFromUtf8(std::string(id) + ".json");
}

std::filesystem::path themePath(const std::filesystem::path& assetRoot, std::string_view id) {
    return assetRoot / "world" / "themes" / core::pathFromUtf8(std::string(id) + ".json");
}

std::string colour(const ObjectReader& reader, std::string_view key) {
    return reader.checkedString(key, isSrgbHexColor, kColourForm);
}

LightStyle readStyle(const ObjectReader& roles, std::string_view role, bool spot) {
    LightStyle style;
    if (spot) {
        const ObjectReader reader = roles.object(role, {"color", "coneMarginDeg", "innerConeRatio", "intensity",
                                                        "minOuterConeDeg", "range"});
        style.color = colour(reader, "color");
        style.intensity = reader.number("intensity", 0.0, kMaxIntensity);
        style.range = reader.number("range", 0.01, kMaxRange);
        style.minOuterConeDeg = reader.number("minOuterConeDeg", 1.0, kMaxOuterConeDeg);
        style.coneMarginDeg = reader.number("coneMarginDeg", 0.0, kMaxOuterConeDeg);
        style.innerConeRatio = reader.number("innerConeRatio", 0.0, 1.0);
    } else {
        const ObjectReader reader = roles.object(role, {"color", "intensity", "range"});
        style.color = colour(reader, "color");
        style.intensity = reader.number("intensity", 0.0, kMaxIntensity);
        style.range = reader.number("range", 0.01, kMaxRange);
        style.minOuterConeDeg = 0.0;
        style.coneMarginDeg = 0.0;
        style.innerConeRatio = 0.0;
    }
    return style;
}

glm::dvec3 readPoint(const ObjectReader& reader, std::string_view key) {
    const Json& value = reader.array(key);
    if (reader.document().failed()) {
        return glm::dvec3(0.0);
    }
    if (value.size() != 3 || !std::all_of(value.begin(), value.end(), [](const Json& v) { return v.is_number(); })) {
        reader.document().fail(reader.pointer(key), "expected three numbers (meters)");
        return glm::dvec3(0.0);
    }
    const glm::dvec3 point(value[0].get<double>(), value[1].get<double>(), value[2].get<double>());
    for (int i = 0; i < 3; ++i) {
        if (!(point[i] >= -10.0 && point[i] <= 10.0)) {
            reader.document().fail(reader.pointer(key), "expected coordinates from -10 to 10 m");
        }
    }
    return point;
}

AccentLight readAccent(DocumentReader& document, const Json& value, const std::string& pointer) {
    AccentLight accent;
    const ObjectReader reader(document, value, pointer,
                              {"anchor", "color", "innerConeDeg", "intensity", "name", "on", "outerConeDeg",
                               "position", "range", "target", "type"});
    accent.name = reader.dataId("name");
    if (reader.has("on")) {
        const std::string on = reader.string("on");
        if (on == "display") {
            accent.on = AccentAnchor::Display;
        } else if (!document.failed() && on != "hero-corner") {
            document.fail(reader.pointer("on"), "expected hero-corner or display, found \"" + on + "\"");
        }
    }
    accent.anchor = reader.dataId("anchor");
    const std::string type = reader.string("type");
    if (!document.failed() && type != "point" && type != "spot") {
        document.fail(reader.pointer("type"), "expected point or spot, found \"" + type + "\"");
    }
    accent.spot = type == "spot";
    accent.position = readPoint(reader, "position");
    accent.color = colour(reader, "color");
    accent.intensity = reader.number("intensity", 0.0, kMaxIntensity);
    accent.range = reader.number("range", 0.01, kMaxRange);
    if (accent.spot) {
        accent.target = readPoint(reader, "target");
        accent.outerConeDeg = reader.number("outerConeDeg", 1.0, kMaxOuterConeDeg);
        accent.innerConeDeg = reader.number("innerConeDeg", 0.0, accent.outerConeDeg);
        if (!document.failed() && accent.target == accent.position) {
            document.fail(reader.pointer("target"), "must differ from the position");
        }
    } else if (reader.has("target") || reader.has("outerConeDeg") || reader.has("innerConeDeg")) {
        document.fail(pointer, "a point light has no target and no cones");
    }
    return accent;
}

bool isMaterialName(std::string_view text) noexcept {
    if (text.size() < 5 || text.size() > 64 || text.substr(0, 4) != "MAT_") {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

/// {"<name>": {"<role>": "<material>"}}: names are data ids, roles plain lower-case words, materials MAT_ names.
std::map<std::string, std::map<std::string, std::string>, std::less<>> readMaterialTable(DocumentReader& document,
                                                                                      const Json& value,
                                                                                      const std::string& pointer) {
    std::map<std::string, std::map<std::string, std::string>, std::less<>> table;
    if (document.failed()) {
        return table;
    }
    if (!value.is_object()) {
        document.fail(pointer, "expected an object, found " + detail::describeType(value));
        return table;
    }
    for (const auto& [name, roles] : value.items()) {
        const std::string namePointer = detail::pointerChild(pointer, name);
        if (!isValidDataId(name)) {
            document.fail(namePointer, "expected a data id");
            return table;
        }
        if (!roles.is_object() || roles.empty()) {
            document.fail(namePointer, "expected an object of material roles");
            return table;
        }
        for (const auto& [role, material] : roles.items()) {
            const std::string rolePointer = detail::pointerChild(namePointer, role);
            if (!isValidDataId(role)) {
                document.fail(rolePointer, "expected a material role (a data id)");
                return table;
            }
            if (!material.is_string() || !isMaterialName(material.get<std::string>())) {
                document.fail(rolePointer, "expected a library material name MAT_<letters, digits, _>");
                return table;
            }
            table[name][role] = material.get<std::string>();
        }
    }
    return table;
}

LightingProfile builtInEvening() {
    LightingProfile p;
    p.name = "evening";
    // The test room's ceiling key (70 at 3.4 m, cones 24 / 44 degrees): grounds the hero wall with soft shadows.
    p.key = LightStyle{"#FFE0C0", 70.0, 12.0, 30.0, 3.0, 0.55};
    // Its pendant fill over the circulation band.
    p.pendant = LightStyle{"#FFE0C0", 16.0, 10.0, 0.0, 0.0, 0.0};
    // The amber practicals (the floor lamp's bulb, a little brighter than the test room's, which had two more spots).
    p.lamp = LightStyle{"#FFC88A", 4.0, 4.0, 0.0, 0.0, 0.0};
    // A warm spot on the sign (the test room's poster wash, nearer).
    p.picture = LightStyle{"#FFE0C0", 8.0, 4.0, 25.0, 10.0, 0.6};
    // The shelf spots (9 to 11 at 2.2 m, cones 18 / 32 degrees): the whole module's height in the cone.
    p.wallWasher = LightStyle{"#FFE0C0", 10.0, 5.0, 32.0, 8.0, 0.56};
    p.keyShadowStrength = 1.0;
    // The environment and the halls as evening.json has them (I6 step 1; unit-tested equal).
    p.ambientSky = "#E6E2DC";
    p.ambientGround = "#B49C88";
    p.ambientIntensity = 0.3;
    p.fillColor = "#FFE0C8";
    p.fillIntensity = 0.22;
    p.hall.light = LightStyle{"#FFD0A0", 12.0, 9.0, 0.0, 0.0, 0.0};
    p.hall.doorwayFill = LightStyle{"#FFD6AE", 3.5, 6.0, 28.0, 6.0, 0.7};
    p.hall.environment = AreaLook{1.5, "#8A8078", "#BCA08A", 0.38, "#FFDCC0", 0.15};
    p.hall.lobbyEnvironment = AreaLook{1.5, "#8A8078", "#BCA08A", 0.45, "#FFDCC0", 0.15};
    return p;
}

/// A copy of assets/world/themes/classic_16bit_warm.json (unit-tested equal), for when it does not load.
ThemeMaterials builtInTheme() {
    using Roles = std::map<std::string, std::string>;
    ThemeMaterials theme;
    theme.id = "classic_16bit_warm";
    theme.kinds = {
        {"room_shell",
         Roles{{"floor", "MAT_Floor_Parquet"},
               {"wall", "MAT_Wall_Painted"},
               {"ceiling", "MAT_Wall_Painted"},
               {"trim", "MAT_Wood_Walnut"},
               {"wainscot", "MAT_Wood_Varnished_Dark"}}},
        {"shelf", Roles{{"wood_primary", "MAT_Wood_Walnut"}, {"wood_secondary", "MAT_Wood_Varnished_Dark"}}},
        {"wall_shelf", Roles{{"wood_primary", "MAT_Wood_Walnut"}, {"wood_secondary", "MAT_Wood_Varnished_Dark"}}},
        {"crt",
         Roles{{"plastic_body", "MAT_Plastic_AgedIvory"},
               {"plastic_bezel", "MAT_Plastic_Black"},
               {"screen", "MAT_Screen_CRT_Room"},
               {"plastic_dark", "MAT_Plastic_Black"}}},
        {"tv_cabinet",
         Roles{{"wood_primary", "MAT_Wood_Walnut"},
               {"wood_secondary", "MAT_Wood_Varnished_Dark"},
               {"metal_trim", "MAT_Metal_Brushed"}}},
        {"console_cartridge",
         Roles{{"plastic_body", "MAT_Plastic_Gray"},
               {"plastic_dark", "MAT_Plastic_Black"},
               {"plastic_accent", "MAT_Plastic_Accent"}}},
        {"poster_frame", Roles{{"frame", "MAT_Wood_Oak"}, {"picture", "MAT_Paper_Print"}}},
        {"rug", Roles{{"fabric", "MAT_Carpet_Ornate"}}},
        {"floor_lamp", Roles{{"metal", "MAT_Metal_Graphite"}, {"lamp_shade", "MAT_Lamp_Shade"}}},
        {"armchair", Roles{{"fabric", "MAT_Fabric_Sofa"}, {"wood", "MAT_Wood_Walnut"}}},
        {"display_case",
         Roles{{"wood_primary", "MAT_Wood_Oak"},
               {"wood_secondary", "MAT_Wood_Varnished_Dark"},
               {"metal_trim", "MAT_Metal_Brushed"},
               {"fabric", "MAT_Fabric_Display"},
               {"glass", "MAT_Glass_Display"}}},
        {"storage_box", Roles{{"cardboard", "MAT_Cardboard_Box"}, {"label", "MAT_Paper_Print"}}},
        {"magazine_stack", Roles{{"paper", "MAT_Paper_Magazine"}, {"cover", "MAT_Label_Print"}}},
        {"bookends",
         Roles{{"metal", "MAT_Metal_Graphite"}, {"book", "MAT_Book_Cloth"}, {"book_alt", "MAT_Book_Cloth_Alt"}}},
        {"plant",
         Roles{{"pot", "MAT_Ceramic_Glazed"},
               {"soil", "MAT_Soil"},
               {"leaves", "MAT_Plant_Leaf"},
               {"leaves_alt", "MAT_Plant_Leaf_Light"}}},
        {"riser_card", Roles{{"frame", "MAT_Paper_Print"}, {"picture", "MAT_Label_Print"}}},
        {"cartridge",
         Roles{{"plastic_shell", "MAT_Plastic_Gray"}, {"label", "MAT_Label_Cover"}, {"spine", "MAT_Label_Cover"}}},
        {"hall",
         Roles{{"floor", "MAT_Floor_OakPlanks"},
               {"wall", "MAT_Plaster_Warm"},
               {"ceiling", "MAT_Plaster_Ceiling"},
               {"trim", "MAT_Wood_Walnut"},
               {"baseboard", "MAT_Wood_Walnut"},
               {"wainscot", "MAT_Wood_Varnished_Dark"},
               {"runner", "MAT_Carpet_Ornate"},
               {"beam", "MAT_Wood_Walnut"},
               {"bench", "MAT_Wood_Walnut"},
               {"cushion", "MAT_Fabric_Sofa"},
               {"plant", "MAT_Ceramic_Glazed"},
               {"saucer", "MAT_Ceramic_Saucer"},
               {"picture", "MAT_Wood_Walnut"},
               {"sconce", "MAT_Sconce_Shade"},
               {"wash", "MAT_Wall_Wash"},
               {"sign", "MAT_Sign_Tungsten"}}},
    };
    theme.heroParts = {{"sign", Roles{{"frame", "MAT_Wood_Walnut"}, {"picture", "MAT_Neon_Diffuser"}}}};
    return theme;
}

} // namespace

AreaLook LightingProfile::roomLook() const {
    return AreaLook{exposure, ambientSky, ambientGround, ambientIntensity, fillColor, fillIntensity};
}

const LightStyle& LightingProfile::style(LightRole role) const noexcept {
    switch (role) {
    case LightRole::Key:
        return key;
    case LightRole::Pendant:
        return pendant;
    case LightRole::Lamp:
        return lamp;
    case LightRole::Picture:
        return picture;
    case LightRole::WallWasher:
        return wallWasher;
    }
    return key;
}

const LightingProfile& eveningLightingProfile() {
    static const LightingProfile profile = builtInEvening();
    return profile;
}

Expected<LightingProfile> parseLightingProfile(std::string_view text, std::string_view context) {
    auto value = detail::parseDataJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    DocumentReader document(text, std::string(context));
    const ObjectReader reader(document, *value, "",
                              {"accents", "environment", "hall", "id", "keyShadowStrength", "name", "roles",
                               "schemaVersion"});
    LightingProfile profile;
    (void)reader.integer("schemaVersion", 1, 1);
    profile.name = reader.dataId("id");
    if (!document.failed() && reader.string("name").empty()) {
        document.fail(reader.pointer("name"), "must not be empty");
    }
    const ObjectReader roles = reader.object("roles", {"key", "lamp", "pendant", "picture", "wallWasher"});
    profile.key = readStyle(roles, "key", true);
    profile.pendant = readStyle(roles, "pendant", false);
    profile.lamp = readStyle(roles, "lamp", false);
    profile.picture = readStyle(roles, "picture", true);
    profile.wallWasher = readStyle(roles, "wallWasher", true);
    profile.keyShadowStrength = reader.number("keyShadowStrength", 0.0, 1.0);
    const ObjectReader environment =
        reader.object("environment", {"ambientGround", "ambientIntensity", "ambientSky", "bloomRadius",
                                      "bloomStrength", "clearColor", "exposure", "fillColor", "fillIntensity", "ssao",
                                      "vignette"});
    profile.clearColor = colour(environment, "clearColor");
    profile.ambientSky = colour(environment, "ambientSky");
    profile.ambientGround = colour(environment, "ambientGround");
    profile.ambientIntensity = environment.number("ambientIntensity", 0.0, 10.0);
    profile.fillColor = colour(environment, "fillColor");
    profile.fillIntensity = environment.number("fillIntensity", 0.0, 10.0);
    profile.exposure = environment.number("exposure", 0.01, 100.0);
    profile.bloomStrength = environment.number("bloomStrength", 0.0, 1.0);
    profile.bloomRadius = environment.number("bloomRadius", 0.0, 1.0);
    profile.vignette = environment.number("vignette", 0.0, 1.0);
    const ObjectReader ssao = environment.object("ssao", {"enabled", "intensity", "radius"});
    profile.ssaoEnabled = ssao.boolean("enabled");
    profile.ssaoRadius = ssao.number("radius", 0.01, 5.0);
    profile.ssaoIntensity = ssao.number("intensity", 0.0, 4.0);
    const ObjectReader hall = reader.object("hall", {"doorwayFill", "environment", "light", "lobbyEnvironment"});
    profile.hall.light = readStyle(hall, "light", false);
    profile.hall.doorwayFill = readStyle(hall, "doorwayFill", true);
    const auto readArea = [&](std::string_view key) {
        const ObjectReader area = hall.object(
            key, {"ambientGround", "ambientIntensity", "ambientSky", "exposure", "fillColor", "fillIntensity"});
        AreaLook look;
        look.exposure = area.number("exposure", 0.01, 100.0);
        look.ambientSky = colour(area, "ambientSky");
        look.ambientGround = colour(area, "ambientGround");
        look.ambientIntensity = area.number("ambientIntensity", 0.0, 10.0);
        look.fillColor = colour(area, "fillColor");
        look.fillIntensity = area.number("fillIntensity", 0.0, 10.0);
        return look;
    };
    profile.hall.environment = readArea("environment");
    profile.hall.lobbyEnvironment = hall.has("lobbyEnvironment") ? readArea("lobbyEnvironment")
                                                                  : profile.hall.environment;
    if (reader.has("accents")) {
        const Json& accents = reader.array("accents");
        detail::forEachElement(accents, reader.pointer("accents"), [&](const Json& element, const std::string& at) {
            if (!document.failed()) {
                profile.accents.push_back(readAccent(document, element, at));
            }
        });
        for (std::size_t i = 0; i < profile.accents.size() && !document.failed(); ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (profile.accents[j].name == profile.accents[i].name) {
                    document.fail(detail::pointerChild(reader.pointer("accents"), i), "a name that appears twice");
                    break;
                }
            }
        }
    }
    if (document.failed()) {
        return propagate(document.error());
    }
    return profile;
}

Expected<LightingProfile> loadLightingProfile(const std::filesystem::path& assetRoot, std::string_view id) {
    if (!isValidDataId(id)) {
        return makeError(ErrorCode::InvalidArgument, "not a lighting profile id", std::string(id));
    }
    const std::filesystem::path path = lightingPath(assetRoot, id);
    auto text = detail::readDataFile(path);
    if (!text) {
        return propagate(text.error());
    }
    const std::string context = core::pathToUtf8Lossy(path);
    auto profile = parseLightingProfile(*text, context);
    if (profile && profile->name != id) {
        return makeError(ErrorCode::ValidationFailed,
                         "/id: the profile's id \"" + profile->name + "\" is not its file's name \"" + std::string(id) +
                             "\"",
                         context);
    }
    return profile;
}

std::vector<std::string> lightingProfileIds(const std::filesystem::path& assetRoot) {
    std::vector<std::string> ids;
    std::error_code error;
    for (std::filesystem::directory_iterator it(assetRoot / "world" / "lighting", error), end; !error && it != end;
         it.increment(error)) {
        const std::filesystem::path& path = it->path();
        if (path.extension() != ".json") {
            continue;
        }
        if (auto stem = core::tryPathToUtf8(path.stem()); stem && isValidDataId(*stem)) {
            ids.push_back(*stem);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

LightingProfile roomLightingProfile(const std::filesystem::path& assetRoot,
                                    const std::filesystem::path& userConfigDir) {
    std::string chosen(kDefaultLightingProfile);
    if (auto config = loadWorldConfig(assetRoot, userConfigDir)) {
        chosen = config->config.lightingProfile;
    }
    auto profile = loadLightingProfile(assetRoot, chosen);
    if (profile) {
        return std::move(*profile);
    }
    spdlog::warn("World: the lighting profile \"{}\" does not load: {}", chosen, profile.error().toString());
    if (chosen != kDefaultLightingProfile) {
        if (auto fallback = loadLightingProfile(assetRoot, kDefaultLightingProfile)) {
            spdlog::warn("World: the room is lit by the default profile \"{}\"", kDefaultLightingProfile);
            return std::move(*fallback);
        }
    }
    spdlog::warn("World: the room is lit by the built-in evening profile");
    return eveningLightingProfile();
}

std::map<std::string, std::string> ThemeMaterials::materials(std::string_view kind, std::string_view heroPart) const {
    if (!heroPart.empty()) {
        if (const auto part = heroParts.find(heroPart); part != heroParts.end()) {
            return part->second;
        }
    }
    if (const auto found = kinds.find(kind); found != kinds.end()) {
        return found->second;
    }
    return {};
}

Expected<ThemeMaterials> parseThemeMaterials(std::string_view text, std::string_view context) {
    auto value = detail::parseDataJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    DocumentReader document(text, std::string(context));
    const ObjectReader reader(document, *value, "", {"heroParts", "id", "kinds", "name", "schemaVersion"});
    ThemeMaterials theme;
    (void)reader.integer("schemaVersion", 1, 1);
    theme.id = reader.dataId("id");
    if (!document.failed() && reader.string("name").empty()) {
        document.fail(reader.pointer("name"), "must not be empty");
    }
    theme.kinds = readMaterialTable(document, reader.value("kinds"), reader.pointer("kinds"));
    theme.heroParts = readMaterialTable(document, reader.value("heroParts"), reader.pointer("heroParts"));
    if (document.failed()) {
        return propagate(document.error());
    }
    return theme;
}

Expected<ThemeMaterials> loadThemeMaterials(const std::filesystem::path& assetRoot, std::string_view id) {
    if (!isValidDataId(id)) {
        return makeError(ErrorCode::InvalidArgument, "not a theme pack id", std::string(id));
    }
    const std::filesystem::path path = themePath(assetRoot, id);
    auto text = detail::readDataFile(path);
    if (!text) {
        return propagate(text.error());
    }
    const std::string context = core::pathToUtf8Lossy(path);
    auto theme = parseThemeMaterials(*text, context);
    if (theme && theme->id != id) {
        return makeError(ErrorCode::ValidationFailed,
                         "/id: the theme's id \"" + theme->id + "\" is not its file's name \"" + std::string(id) + "\"",
                         context);
    }
    return theme;
}

const ThemeMaterials& builtInThemeMaterials() {
    static const ThemeMaterials theme = builtInTheme();
    return theme;
}

std::map<std::string, std::string> themeMaterials(std::string_view kind) {
    return builtInThemeMaterials().materials(kind);
}

} // namespace rv::world
