#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <rv/assets/glb_writer.hpp>
#include <rv/assets/gltf_loader.hpp>
#include <rv/assets/prefab_manifest.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/geometry/prefabs.hpp>

using rv::ErrorCode;
using rv::assets::PrefabManifest;
using rv::assets::ValidationIssue;
using Severity = rv::assets::ValidationIssue::Severity;

namespace {

const std::filesystem::path kManifestDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR) / "manifests/prefabs";

/// Procedural prefab kinds of the shared geometry contract.
const std::set<std::string> kGeneratorKinds = [] {
    std::set<std::string> kinds;
    for (const std::string_view kind : rv::assets::kProceduralPrefabKinds) {
        kinds.emplace(kind);
    }
    return kinds;
}();

/// The example manifest of style.md section 11, verbatim.
constexpr std::string_view kStyleExample = R"({
  "schemaVersion": 1,
  "id": "electronics.crt.consumer_29.a",
  "assetType": "electronic_screen",
  "category": "crt",
  "role": "hero",
  "source": {
    "kind": "original",
    "url": null,
    "author": "Retroverse team",
    "acquiredAt": "2026-10-14",
    "license": "proprietary-original",
    "licenseSnapshot": null,
    "attributionRequired": false,
    "redistributionApproved": true
  },
  "runtime": {
    "model": "models/electronics/crt_consumer_29.glb",
    "scaleMeters": [0.71, 0.61, 0.56],
    "lods": ["crt_consumer_29.glb", "crt_consumer_29_lod1.glb"],
    "defaultMaterialVariant": "warm_graphite",
    "sockets": {
      "screen": "ScreenSurface",
      "inspect": "Anchor_ScreenLook",
      "power": "PowerButton"
    },
    "collision": "box",
    "videoSurface": {"aspectRatio": "4:3", "meshNode": "ScreenSurface"}
  },
  "tags": ["crt", "1990s", "tv", "console_room"],
  "fallbackPrefab": "electronics.crt.generic",
  "hash": "sha256:TO_BE_GENERATED_BY_BUILD"
})";

nlohmann::json json(std::string_view text) {
    auto parsed = rv::core::parseJson(text, "test");
    REQUIRE(parsed.has_value());
    return *parsed;
}

PrefabManifest example() {
    auto manifest = rv::assets::parsePrefabManifest(json(kStyleExample), "example.json");
    REQUIRE(manifest.has_value());
    return *manifest;
}

std::vector<std::string> errors(const std::vector<ValidationIssue>& issues) {
    std::vector<std::string> messages;
    for (const ValidationIssue& issue : issues) {
        if (issue.severity == Severity::Error) {
            messages.push_back(issue.message);
        }
    }
    return messages;
}

bool hasIssue(const std::vector<ValidationIssue>& issues, Severity severity, std::string_view prefix) {
    return std::any_of(issues.begin(), issues.end(), [&](const ValidationIssue& issue) {
        return issue.severity == severity && issue.message.starts_with(prefix);
    });
}

rv::geometry::Socket socket(std::string id, std::string nodeName, rv::geometry::SocketKind kind) {
    rv::geometry::Socket result;
    result.id = std::move(id);
    result.nodeName = std::move(nodeName);
    result.kind = kind;
    return result;
}

/// Sockets of the procedural CRT of the shared geometry contract.
std::vector<rv::geometry::Socket> proceduralCrtSockets() {
    using rv::geometry::SocketKind;
    return {socket("ScreenSurface", "ScreenSurface", SocketKind::ScreenSurface),
            socket("TV_Stand", "Anchor_TV_Stand", SocketKind::Anchor),
            socket("ScreenLook", "Anchor_ScreenLook", SocketKind::Anchor),
            socket("PowerButton", "PowerButton", SocketKind::Other)};
}

} // namespace

TEST_CASE("assets/manifest: every shipped prefab manifest parses and validates", "[assets][manifest]") {
    REQUIRE(std::filesystem::is_directory(kManifestDir));
    std::set<std::string> generators;
    std::set<std::string> ids;
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(kManifestDir)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        ++count;
        const std::string fileName = rv::core::pathToUtf8(entry.path().filename());
        CAPTURE(fileName);
        const auto manifest = rv::assets::loadPrefabManifest(entry.path());
        REQUIRE(manifest.has_value());
        CHECK(fileName == manifest->id + ".json");
        CHECK(ids.insert(manifest->id).second);

        const auto issues = rv::assets::validatePrefabManifest(*manifest);
        CHECK(errors(issues).empty());
        CHECK(hasIssue(issues, Severity::Warning, "hash:")); // placeholder until the asset build hashes files

        REQUIRE(manifest->runtime.generator.has_value());
        CHECK_FALSE(manifest->runtime.model.has_value());
        CHECK(kGeneratorKinds.contains(*manifest->runtime.generator));
        CHECK(manifest->runtime.params.is_object());
        generators.insert(*manifest->runtime.generator);

        CHECK(manifest->source.kind == "original");
        CHECK(manifest->source.author == "Retroverse contributors");
        CHECK(manifest->source.license == "MIT");
        CHECK((manifest->source.acquiredAt == "2026-10-14" || manifest->source.acquiredAt == "2026-10-18" ||
               manifest->source.acquiredAt == "2026-10-19" || manifest->source.acquiredAt == "2026-10-20" ||
               manifest->source.acquiredAt == "2026-10-24" || manifest->source.acquiredAt == "2026-10-25" ||
               manifest->source.acquiredAt == "2026-10-26"));
        CHECK(manifest->source.redistributionApproved);
        CHECK_FALSE(manifest->tags.empty());
        if (!manifest->fallbackPrefab.empty()) {
            CHECK(std::filesystem::exists(kManifestDir / (manifest->fallbackPrefab + ".json")));
        }
    }
    // At least one manifest per procedural prefab kind; the shelf has three, the generic one and the collection's
    // hero-wall and side-wall modules (I3 step 1b); the floor lamp, the TV cabinet and the poster frame two each, the
    // slim lamp, the narrow cabinet and the room's sign (I3 step 16); the eight media families of I4 step 2 an item
    // and four fixtures each (ADR-0013 §4); the five successors of I6 step 4 an item and two modules each (their display
    // fixtures are their predecessors', ADR-0014 §2); I6 step 3b's era furniture and I6 step 5's hub (column, railing,
    // landmark, totem) one each, counted with the kinds.
    CHECK(count == kGeneratorKinds.size() + 5 + 8 * 5 + 5 * 3);
    CHECK(generators == kGeneratorKinds);
    // Every kind the geometry builds is a manifest kind.
    const auto built = rv::geometry::prefabKinds();
    CHECK(std::set<std::string>(built.begin(), built.end()) == kGeneratorKinds);
}

TEST_CASE("assets/manifest: the procedural CRT manifest matches the contract sockets", "[assets][manifest]") {
    const auto manifest = rv::assets::loadPrefabManifest(kManifestDir / "electronics.crt.consumer.generic.json");
    REQUIRE(manifest.has_value());
    CHECK(manifest->assetType == "electronic_screen");
    CHECK(manifest->runtime.generator == std::optional<std::string>("crt"));
    CHECK(manifest->runtime.sockets ==
          std::map<std::string, std::string>{
              {"screen", "ScreenSurface"}, {"inspect", "ScreenLook"}, {"stand", "TV_Stand"}, {"power", "PowerButton"}});
    REQUIRE(manifest->runtime.videoSurface.has_value());
    CHECK(manifest->runtime.videoSurface->aspectRatio == "4:3");
    CHECK(manifest->runtime.videoSurface->meshNode == "ScreenSurface");

    const auto sockets = proceduralCrtSockets();
    CHECK(errors(rv::assets::validatePrefabManifest(*manifest, nullptr, sockets)).empty());

    // A procedural CRT without its power button socket breaks the manifest.
    const std::vector<rv::geometry::Socket> incomplete(sockets.begin(), sockets.begin() + 3);
    const auto issues = rv::assets::validatePrefabManifest(*manifest, nullptr, incomplete);
    CHECK(errors(issues) == std::vector<std::string>{"runtime.sockets.power: socket \"PowerButton\" does not exist"});
}

TEST_CASE("assets/manifest: an empty procedural socket list is still a socket source", "[assets][manifest]") {
    auto rug = rv::assets::loadPrefabManifest(kManifestDir / "decor.rug.generic.json");
    REQUIRE(rug.has_value());
    const std::vector<rv::geometry::Socket> none; // the procedural rug has no sockets
    CHECK(errors(rv::assets::validatePrefabManifest(*rug, nullptr, none)).empty());

    rug->runtime.sockets["center"] = "Center";
    CHECK(errors(rv::assets::validatePrefabManifest(*rug, nullptr, none)) ==
          std::vector<std::string>{"runtime.sockets.center: socket \"Center\" does not exist"});
    // Without a model or procedural sockets, socket names cannot be checked.
    CHECK(errors(rv::assets::validatePrefabManifest(*rug)).empty());
}

TEST_CASE("assets/manifest: the style section 11 example parses", "[assets][manifest]") {
    const PrefabManifest manifest = example();
    CHECK(manifest.schemaVersion == 1);
    CHECK(manifest.id == "electronics.crt.consumer_29.a");
    CHECK(manifest.assetType == "electronic_screen");
    CHECK(manifest.category == "crt");
    CHECK(manifest.role == "hero");
    CHECK(manifest.source.url.empty());
    CHECK(manifest.source.licenseSnapshot.empty());
    CHECK(manifest.source.license == "proprietary-original");
    CHECK_FALSE(manifest.source.attributionRequired);
    CHECK(manifest.runtime.model == std::optional<std::string>("models/electronics/crt_consumer_29.glb"));
    CHECK_FALSE(manifest.runtime.generator.has_value());
    REQUIRE(manifest.runtime.scaleMeters.has_value());
    CHECK(*manifest.runtime.scaleMeters == glm::vec3(0.71f, 0.61f, 0.56f));
    CHECK(manifest.runtime.lods == std::vector<std::string>{"crt_consumer_29.glb", "crt_consumer_29_lod1.glb"});
    CHECK(manifest.runtime.defaultMaterialVariant == "warm_graphite");
    CHECK(manifest.runtime.sockets.at("inspect") == "Anchor_ScreenLook");
    CHECK(manifest.runtime.collision == "box");
    CHECK(manifest.tags.size() == 4);
    CHECK(manifest.fallbackPrefab == "electronics.crt.generic");
    CHECK(manifest.hash == rv::assets::kPlaceholderHash);

    const auto issues = rv::assets::validatePrefabManifest(manifest);
    CHECK(errors(issues).empty());
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].severity == Severity::Warning);
}

TEST_CASE("assets/manifest: validation against a loaded model", "[assets][manifest]") {
    rv::assets::GlbDescription description;
    description.meshes.push_back(rv::assets::makeQuadMesh("screen", 0.48f, 0.36f));
    rv::assets::GlbNode root;
    root.name = "Root";
    root.children = {1, 2, 3};
    rv::assets::GlbNode screen;
    screen.name = "ScreenSurface";
    screen.mesh = 0;
    rv::assets::GlbNode look;
    look.name = "Anchor_ScreenLook";
    rv::assets::GlbNode power;
    power.name = "PowerButton";
    description.nodes = {root, screen, look, power};
    const auto glb = rv::assets::buildGlb(description);
    REQUIRE(glb.has_value());
    const auto model = rv::assets::loadGltfFromMemory(*glb, {});
    REQUIRE(model.has_value());

    const PrefabManifest manifest = example();
    CHECK(errors(rv::assets::validatePrefabManifest(manifest, &*model)).empty());

    PrefabManifest wrongScreen = manifest;
    wrongScreen.runtime.sockets["screen"] = "PowerButton";
    CHECK(
        errors(rv::assets::validatePrefabManifest(wrongScreen, &*model)) ==
        std::vector<std::string>{"runtime.sockets.screen: \"PowerButton\" is not a ScreenSurface socket (kind other)"});

    PrefabManifest missing = manifest;
    missing.runtime.sockets["inspect"] = "Anchor_Missing";
    CHECK(errors(rv::assets::validatePrefabManifest(missing, &*model)) ==
          std::vector<std::string>{"runtime.sockets.inspect: socket \"Anchor_Missing\" does not exist"});

    PrefabManifest wrongSurface = manifest;
    wrongSurface.runtime.videoSurface->meshNode = "ScreenLook";
    CHECK(errors(rv::assets::validatePrefabManifest(wrongSurface, &*model)) ==
          std::vector<std::string>{
              "runtime.videoSurface.meshNode: \"ScreenLook\" is not a ScreenSurface socket (kind anchor)"});

    // Without a model or procedural sockets, socket names cannot be checked.
    CHECK(errors(rv::assets::validatePrefabManifest(missing)).empty());
}

TEST_CASE("assets/manifest: every validation rule has a failing case", "[assets][manifest]") {
    struct Case {
        const char* name;
        std::function<void(PrefabManifest&)> edit;
        const char* field;
    };
    const std::vector<Case> cases = {
        {"id with one segment", [](PrefabManifest& m) { m.id = "crt"; }, "id:"},
        {"id with uppercase", [](PrefabManifest& m) { m.id = "Electronics.crt"; }, "id:"},
        {"id with an empty segment", [](PrefabManifest& m) { m.id = "electronics..crt"; }, "id:"},
        {"id ending with a dot", [](PrefabManifest& m) { m.id = "electronics.crt."; }, "id:"},
        {"id with a dash", [](PrefabManifest& m) { m.id = "electronics.crt-29"; }, "id:"},
        {"empty assetType", [](PrefabManifest& m) { m.assetType.clear(); }, "assetType:"},
        {"empty category", [](PrefabManifest& m) { m.category.clear(); }, "category:"},
        {"model and generator", [](PrefabManifest& m) { m.runtime.generator = "crt"; }, "runtime:"},
        {"neither model nor generator", [](PrefabManifest& m) { m.runtime.model.reset(); }, "runtime:"},
        {"model escaping the asset root", [](PrefabManifest& m) { m.runtime.model = "../models/crt.glb"; },
         "runtime.model:"},
        {"model that is not glTF", [](PrefabManifest& m) { m.runtime.model = "models/crt.fbx"; }, "runtime.model:"},
        {"invalid generator",
         [](PrefabManifest& m) {
             m.runtime.model.reset();
             m.runtime.generator = "CRT TV";
         },
         "runtime.generator:"},
        {"unknown generator kind",
         [](PrefabManifest& m) {
             m.runtime.model.reset();
             m.runtime.generator = "crtt";
         },
         "runtime.generator:"},
        {"absolute LOD path", [](PrefabManifest& m) { m.runtime.lods = {"/lod1.glb"}; }, "runtime.lods[0]:"},
        {"zero scale", [](PrefabManifest& m) { m.runtime.scaleMeters = glm::vec3(0.71f, 0.0f, 0.56f); },
         "runtime.scaleMeters:"},
        {"negative scale", [](PrefabManifest& m) { m.runtime.scaleMeters = glm::vec3(-1.0f, 1.0f, 1.0f); },
         "runtime.scaleMeters:"},
        {"electronic_screen without a screen socket", [](PrefabManifest& m) { m.runtime.sockets.erase("screen"); },
         "runtime.sockets.screen:"},
        {"electronic_screen without a video surface", [](PrefabManifest& m) { m.runtime.videoSurface.reset(); },
         "runtime.videoSurface:"},
        {"malformed aspect ratio", [](PrefabManifest& m) { m.runtime.videoSurface->aspectRatio = "wide"; },
         "runtime.videoSurface.aspectRatio:"},
        {"zero aspect ratio", [](PrefabManifest& m) { m.runtime.videoSurface->aspectRatio = "4:0"; },
         "runtime.videoSurface.aspectRatio:"},
        {"empty video mesh node", [](PrefabManifest& m) { m.runtime.videoSurface->meshNode.clear(); },
         "runtime.videoSurface.meshNode:"},
        {"empty license", [](PrefabManifest& m) { m.source.license.clear(); }, "source.license:"},
        {"empty source kind", [](PrefabManifest& m) { m.source.kind.clear(); }, "source.kind:"},
        {"redistribution not approved", [](PrefabManifest& m) { m.source.redistributionApproved = false; },
         "source.redistributionApproved:"},
        {"malformed hash", [](PrefabManifest& m) { m.hash = "md5:1234"; }, "hash:"},
        {"short hash", [](PrefabManifest& m) { m.hash = "sha256:" + std::string(63, 'a'); }, "hash:"},
        {"non-hex hash", [](PrefabManifest& m) { m.hash = "sha256:" + std::string(64, 'g'); }, "hash:"},
        {"empty hash", [](PrefabManifest& m) { m.hash.clear(); }, "hash:"},
        {"malformed fallback", [](PrefabManifest& m) { m.fallbackPrefab = "Generic CRT"; }, "fallbackPrefab:"},
        {"self fallback", [](PrefabManifest& m) { m.fallbackPrefab = m.id; }, "fallbackPrefab:"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        PrefabManifest manifest = example();
        c.edit(manifest);
        const auto issues = rv::assets::validatePrefabManifest(manifest);
        CHECK(hasIssue(issues, Severity::Error, c.field));
        CHECK(rv::assets::hasErrors(issues));
    }
}

TEST_CASE("assets/manifest: rules that pass or only warn", "[assets][manifest]") {
    PrefabManifest userAsset = example();
    userAsset.source.kind = "user";
    userAsset.source.redistributionApproved = false;
    CHECK(errors(rv::assets::validatePrefabManifest(userAsset)).empty());

    PrefabManifest hashed = example();
    hashed.hash = "sha256:" + std::string(32, 'a') + std::string(32, 'F');
    CHECK(rv::assets::validatePrefabManifest(hashed).empty());

    PrefabManifest withParams = hashed;
    withParams.runtime.params = {{"width", 0.8}};
    const auto issues = rv::assets::validatePrefabManifest(withParams);
    CHECK(hasIssue(issues, Severity::Warning, "runtime.params:"));
    CHECK_FALSE(rv::assets::hasErrors(issues));

    PrefabManifest decimalAspect = hashed;
    decimalAspect.runtime.videoSurface->aspectRatio = "1.85:1";
    CHECK(rv::assets::validatePrefabManifest(decimalAspect).empty());

    PrefabManifest procedural = hashed;
    procedural.runtime.model.reset();
    procedural.runtime.generator = "crt";
    procedural.runtime.params = {{"bezel", 0.05}};
    CHECK(rv::assets::validatePrefabManifest(procedural).empty());

    CHECK(rv::assets::isValidPrefabId("decor.rug.generic"));
    CHECK(rv::assets::isValidPrefabId("a.b"));
    CHECK_FALSE(rv::assets::isValidPrefabId(""));
    CHECK_FALSE(rv::assets::isValidPrefabId(".a.b"));
}

TEST_CASE("assets/manifest: structural errors carry a JSON path", "[assets][manifest]") {
    const auto parseEdited = [](const std::function<void(nlohmann::json&)>& edit) {
        nlohmann::json document = json(kStyleExample);
        edit(document);
        return rv::assets::parsePrefabManifest(document, "crt.json");
    };
    struct Case {
        const char* name;
        std::function<void(nlohmann::json&)> edit;
        ErrorCode code;
        const char* context;
    };
    const std::vector<Case> cases = {
        {"unknown key", [](nlohmann::json& d) { d["colour"] = "red"; }, ErrorCode::ParseError, "crt.json:/colour"},
        {"unknown runtime key", [](nlohmann::json& d) { d["runtime"]["scale"] = 1; }, ErrorCode::ParseError,
         "crt.json:/runtime/scale"},
        {"unknown source key", [](nlohmann::json& d) { d["source"]["licence"] = "MIT"; }, ErrorCode::ParseError,
         "crt.json:/source/licence"},
        {"missing id", [](nlohmann::json& d) { d.erase("id"); }, ErrorCode::ParseError, "crt.json:/id"},
        {"missing runtime", [](nlohmann::json& d) { d.erase("runtime"); }, ErrorCode::ParseError, "crt.json:/runtime"},
        {"schema version 2", [](nlohmann::json& d) { d["schemaVersion"] = 2; }, ErrorCode::Unsupported,
         "crt.json:/schemaVersion"},
        {"id is a number", [](nlohmann::json& d) { d["id"] = 42; }, ErrorCode::ParseError, "crt.json:/id"},
        {"tags is a string", [](nlohmann::json& d) { d["tags"] = "crt"; }, ErrorCode::ParseError, "crt.json:/tags"},
        {"tag is a number", [](nlohmann::json& d) { d["tags"][1] = 1990; }, ErrorCode::ParseError, "crt.json:/tags/1"},
        {"scaleMeters with two values", [](nlohmann::json& d) { d["runtime"]["scaleMeters"] = {1, 2}; },
         ErrorCode::ParseError, "crt.json:/runtime/scaleMeters"},
        {"socket value is a number", [](nlohmann::json& d) { d["runtime"]["sockets"]["screen"] = 3; },
         ErrorCode::ParseError, "crt.json:/runtime/sockets/screen"},
        {"params is an array", [](nlohmann::json& d) { d["runtime"]["params"] = {1, 2}; }, ErrorCode::ParseError,
         "crt.json:/runtime/params"},
        {"redistribution is a string", [](nlohmann::json& d) { d["source"]["redistributionApproved"] = "yes"; },
         ErrorCode::ParseError, "crt.json:/source/redistributionApproved"},
        {"video surface key", [](nlohmann::json& d) { d["runtime"]["videoSurface"]["fps"] = 60; },
         ErrorCode::ParseError, "crt.json:/runtime/videoSurface/fps"},
        {"document is an array", [](nlohmann::json& d) { d = nlohmann::json::array(); }, ErrorCode::ParseError,
         "crt.json:/"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        const auto manifest = parseEdited(c.edit);
        REQUIRE_FALSE(manifest.has_value());
        CHECK(manifest.error().code == c.code);
        CHECK(manifest.error().context == c.context);
    }

    // Nullable fields: null model and generator mean "absent".
    const auto nulls = parseEdited([](nlohmann::json& d) {
        d["runtime"]["model"] = nullptr;
        d["runtime"]["generator"] = "crt";
        d["runtime"]["params"] = nullptr;
        d["role"] = nullptr;
    });
    REQUIRE(nulls.has_value());
    CHECK_FALSE(nulls->runtime.model.has_value());
    CHECK(nulls->runtime.params.is_object());
    CHECK(nulls->role.empty());

    const auto missingFile = rv::assets::loadPrefabManifest(kManifestDir / "does.not.exist.json");
    REQUIRE_FALSE(missingFile.has_value());
    CHECK(missingFile.error().code == ErrorCode::NotFound);
}
