// Object fallbacks (style §7.3, §21, §26.3; I3 step 1b): the prefab families and assets::resolvePrefab.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/log_capture.hpp"
#include "support/temp_dir.hpp"
#include <rv/assets/prefab_manifest.hpp>
#include <rv/assets/prefab_resolver.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/geometry/mesh_data.hpp>
#include <rv/geometry/prefabs.hpp>

using rv::ErrorCode;
using rv::assets::PrefabFamilies;
using rv::assets::PrefabSource;
using rv::assets::ResolvedPrefab;

namespace {

const std::filesystem::path kAssetRoot = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);

nlohmann::json json(std::string_view text) {
    auto parsed = rv::core::parseJson(text, "test");
    REQUIRE(parsed.has_value());
    return *parsed;
}

PrefabFamilies shippedFamilies() {
    auto families = rv::assets::loadPrefabFamilies(kAssetRoot);
    if (!families) {
        FAIL(families.error().toString());
    }
    return std::move(*families);
}

/// The ids of the shipped manifests, sorted.
std::vector<std::string> shippedIds() {
    std::set<std::string> ids;
    for (const auto& entry : std::filesystem::directory_iterator(kAssetRoot / rv::assets::kPrefabManifestDir)) {
        if (entry.path().extension() == ".json") {
            ids.insert(rv::core::pathToUtf8(entry.path().stem()));
        }
    }
    return {ids.begin(), ids.end()};
}

/// A scratch asset root holding a copy of the shipped manifests and families.
struct ScratchRoot {
    rv::test::TempDir dir{"rv-prefab-resolver"};

    ScratchRoot() {
        std::filesystem::create_directories(dir.path() / "manifests");
        std::filesystem::copy(kAssetRoot / "manifests", dir.path() / "manifests",
                              std::filesystem::copy_options::recursive);
    }

    [[nodiscard]] std::filesystem::path manifest(std::string_view id) const {
        return rv::assets::prefabManifestPath(dir.path(), id);
    }

    void write(std::string_view id, std::string_view text) const {
        std::ofstream out(manifest(id), std::ios::binary | std::ios::trunc);
        out << text;
    }

    /// The manifest's JSON changed by `edit`, written back.
    template <class Edit>
    void edit(std::string_view id, Edit&& change) const {
        auto document = rv::core::readJsonFile(manifest(id));
        REQUIRE(document.has_value());
        change(*document);
        write(id, document->dump(2));
    }
};

std::vector<std::uint64_t> hashes(const rv::geometry::ProceduralPrefab& prefab) {
    std::vector<std::uint64_t> list;
    for (const auto& part : prefab.parts) {
        list.push_back(rv::geometry::hashMesh(part.mesh));
    }
    return list;
}

ResolvedPrefab resolveOk(const std::filesystem::path& root, const PrefabFamilies& families, std::string_view id) {
    auto resolved = rv::assets::resolvePrefab(root, families, id);
    if (!resolved) {
        FAIL(std::string(id) + ": " + resolved.error().toString());
    }
    return std::move(*resolved);
}

} // namespace

TEST_CASE("assets/families: every shipped manifest has a family whose fallbacks exist", "[assets][families]") {
    const PrefabFamilies families = shippedFamilies();
    REQUIRE(families.families.size() == 16);
    for (std::size_t i = 1; i < families.families.size(); ++i) {
        CHECK(families.families[i - 1].id < families.families[i].id);
    }
    // Style §26.3: a family fallback exists for every category, and it is the generic manifest's prefab.
    for (const rv::assets::PrefabFamily& family : families.families) {
        INFO(family.id);
        auto generic = rv::assets::loadPrefabManifest(rv::assets::prefabManifestPath(kAssetRoot, family.generic));
        REQUIRE(generic.has_value());
        CHECK(families.familyOf(family.generic) == &family);
        REQUIRE(generic->runtime.generator.has_value());
        CHECK(*generic->runtime.generator == family.generator);
        CHECK(generic->runtime.params == family.params);
        CHECK(rv::geometry::buildPrefab(family.generator, family.params).has_value());
    }
    // Every shipped manifest belongs to a family, and a manifest that names a fallback names its family's generic.
    for (const std::string& id : shippedIds()) {
        INFO(id);
        const rv::assets::PrefabFamily* family = families.familyOf(id);
        REQUIRE(family != nullptr);
        auto manifest = rv::assets::loadPrefabManifest(rv::assets::prefabManifestPath(kAssetRoot, id));
        REQUIRE(manifest.has_value());
        if (id == family->generic) {
            CHECK(manifest->fallbackPrefab.empty());
        } else {
            CHECK(manifest->fallbackPrefab == family->generic);
        }
    }
    // The longest dotted prefix wins; a prefix must end at a dot.
    CHECK(families.familyOf("furniture.shelf.hero_module.generic")->id == "furniture.shelf");
    CHECK(families.familyOf("furniture.shelf_modular.generic")->id == "furniture.shelf_modular");
    CHECK(families.familyOf("media.cartridge.home16.pal") != nullptr);
    CHECK(families.familyOf("media.cartridge_handheld.generic") == nullptr);
    CHECK(families.familyOf("media.cartridge") == nullptr);
    CHECK(families.familyOf("arcade.cabinet.generic") == nullptr);
}

TEST_CASE("assets/families: malformed registries are refused with their JSON pointer", "[assets][families]") {
    struct Case {
        std::string_view text;
        std::string_view context;
    };
    const std::vector<Case> cases{
        {R"({"schemaVersion": 2, "families": []})", "/schemaVersion"},
        {R"({"families": []})", "/schemaVersion"},
        {R"({"schemaVersion": 1})", "/families"},
        {R"({"schemaVersion": 1, "families": {}})", "/families"},
        {R"({"schemaVersion": 1, "families": [], "extra": 1})", "extra"},
        {R"({"schemaVersion": 1, "families": [{"id": "a.b", "generic": "a.b.generic"}]})", "/families/0/generator"},
        {R"({"schemaVersion": 1, "families": [{"id": "a.b", "generic": "a.b.generic", "generator": "boat"}]})",
         "/families/0/generator"},
        {R"({"schemaVersion": 1, "families": [{"id": "A.b", "generic": "a.b.generic", "generator": "rug"}]})",
         "/families/0/id"},
        {R"({"schemaVersion": 1, "families": [{"id": "a.b", "generic": "c.d.generic", "generator": "rug"}]})",
         "/families/0/generic"},
        {R"({"schemaVersion": 1, "families": [{"id": "a.b", "generic": "a.b.x", "generator": "rug", "params": 3}]})",
         "/families/0/params"},
        {R"({"schemaVersion": 1, "families": [{"id": "a.b", "generic": "a.b.x", "generator": "rug", "seed": 3}]})",
         "seed"},
        {R"({"schemaVersion": 1, "families": [{"id": "a.b", "generic": "a.b.x", "generator": "rug"},
                                              {"id": "a.b", "generic": "a.b.y", "generator": "rug"}]})",
         "/families"},
    };
    for (const Case& c : cases) {
        INFO(c.text);
        const auto families = rv::assets::parsePrefabFamilies(json(c.text), "families.json");
        REQUIRE_FALSE(families.has_value());
        INFO(families.error().toString());
        CHECK(families.error().context.find(c.context) != std::string::npos);
    }
    const auto ok = rv::assets::parsePrefabFamilies(
        json(R"({"schemaVersion": 1, "families": [{"id": "a.b", "generic": "a.b.x", "generator": "rug"}]})"), "f");
    REQUIRE(ok.has_value());
    CHECK(ok->families.front().params == nlohmann::json::object());
}

TEST_CASE("assets/resolver: every shipped manifest resolves as itself, silently", "[assets][resolver]") {
    const PrefabFamilies families = shippedFamilies();
    rv::test::LogCapture log;
    for (const std::string& id : shippedIds()) {
        INFO(id);
        const ResolvedPrefab resolved = resolveOk(kAssetRoot, families, id);
        CHECK(resolved.source == PrefabSource::Requested);
        CHECK(resolved.requestedId == id);
        REQUIRE(resolved.manifest.has_value());
        CHECK(resolved.manifest->id == id);
        REQUIRE(resolved.prefab.has_value());
        CHECK(resolved.generator == *resolved.manifest->runtime.generator);
        CHECK(resolved.fallbackReason.empty());
    }
    CHECK(log.text().empty());
}

TEST_CASE("assets/resolver: a removed manifest falls back to its family's prefab with one warning",
          "[assets][resolver]") {
    const ScratchRoot root;
    const PrefabFamilies families = shippedFamilies();
    const ResolvedPrefab generic = resolveOk(root.dir.path(), families, "furniture.shelf.side_module.generic");

    // Style §26.3's "loading with an asset lost on purpose": the hero-wall module's manifest removed.
    std::filesystem::remove(root.manifest("furniture.shelf.hero_module.generic"));
    {
        rv::test::LogCapture log;
        const ResolvedPrefab resolved = resolveOk(root.dir.path(), families, "furniture.shelf.hero_module.generic");
        CHECK(resolved.source == PrefabSource::FamilyGeneric);
        CHECK(resolved.requestedId == "furniture.shelf.hero_module.generic");
        REQUIRE(resolved.manifest.has_value());
        CHECK(resolved.manifest->id == "furniture.shelf.side_module.generic");
        CHECK(resolved.fallbackReason == "the manifest is missing");
        REQUIRE(resolved.prefab.has_value());
        CHECK(hashes(*resolved.prefab) == hashes(*generic.prefab));
        // The fallback fits the module's socket: the same footprint, without the hero band.
        CHECK(resolved.prefab->bounds == generic.prefab->bounds);
        CHECK(log.count("warning") == 1);
        CHECK(log.count("furniture.shelf.hero_module.generic") == 1);
        CHECK(log.count("generic prefab 'furniture.shelf.side_module.generic'") == 1);
    }
    // The generic one removed as well: the family's procedural prefab, the same geometry, one warning.
    std::filesystem::remove(root.manifest("furniture.shelf.side_module.generic"));
    {
        rv::test::LogCapture log;
        const ResolvedPrefab resolved = resolveOk(root.dir.path(), families, "furniture.shelf.hero_module.generic");
        CHECK(resolved.source == PrefabSource::FamilyProcedural);
        CHECK_FALSE(resolved.manifest.has_value());
        CHECK(resolved.generator == "shelf");
        REQUIRE(resolved.prefab.has_value());
        CHECK(hashes(*resolved.prefab) == hashes(*generic.prefab));
        CHECK(resolved.fallbackReason.find("the manifest is missing") != std::string::npos);
        CHECK(log.count("warning") == 1);
        CHECK(log.count("procedural prefab 'shelf'") == 1);
    }
    {
        rv::test::LogCapture log;
        const ResolvedPrefab resolved = resolveOk(root.dir.path(), families, "furniture.shelf.side_module.generic");
        CHECK(resolved.source == PrefabSource::FamilyProcedural);
        CHECK(log.count("warning") == 1);
    }
}

TEST_CASE("assets/resolver: a fallback met again in the same process is not warned about again", "[assets][resolver]") {
    // I3 step 10c: a room resolves its prefabs in several jobs of one run (its capacity, its scene, its persisted
    // start); the first resolution warns, the next ones log at debug level and resolve alike.
    const ScratchRoot root;
    const PrefabFamilies families = shippedFamilies();
    std::filesystem::remove(root.manifest("decor.storage_box.generic"));
    {
        rv::test::LogCapture log;
        const ResolvedPrefab first = resolveOk(root.dir.path(), families, "decor.storage_box.generic");
        CHECK(first.source == PrefabSource::FamilyProcedural);
        CHECK(log.count("warning") == 1);
    }
    for (int again = 0; again < 3; ++again) {
        rv::test::LogCapture log;
        const ResolvedPrefab resolved = resolveOk(root.dir.path(), families, "decor.storage_box.generic");
        CHECK(resolved.source == PrefabSource::FamilyProcedural);
        CHECK(resolved.fallbackReason == "the manifest is missing");
        CHECK(log.count("warning") == 0);
    }
    // Another asset root is another fallback: warned about once again.
    const ScratchRoot other;
    std::filesystem::remove(other.manifest("decor.storage_box.generic"));
    rv::test::LogCapture log;
    static_cast<void>(resolveOk(other.dir.path(), families, "decor.storage_box.generic"));
    CHECK(log.count("warning") == 1);
}

TEST_CASE("assets/resolver: an invalid manifest falls back like a missing one", "[assets][resolver]") {
    const PrefabFamilies families = shippedFamilies();
    struct Case {
        std::string_view name;
        std::string_view id;
        std::string_view reason;
    };
    const ScratchRoot root;
    root.write("decor.bookends.generic", "{ not json");
    root.edit("decor.magazine_stack.generic", [](nlohmann::json& m) { m["runtime"]["params"]["seed"] = -4; });
    root.edit("media.cartridge.home16.generic", [](nlohmann::json& m) { m["id"] = "media.cartridge.other"; });
    root.edit("electronics.crt.consumer.generic", [](nlohmann::json& m) { m["runtime"]["generator"] = "rug"; });
    root.edit("furniture.display_case.generic", [](nlohmann::json& m) { m["runtime"]["sockets"]["glass"] = "Pane"; });
    root.edit("furniture.wall_shelf.featured.generic", [](nlohmann::json& m) { m["source"]["license"] = ""; });
    const std::vector<Case> cases{
        {"broken JSON", "decor.bookends.generic", ""},
        {"parameters out of range", "decor.magazine_stack.generic", "outside"},
        {"another id", "media.cartridge.home16.generic", "names the id"},
        {"a generator without the named sockets", "electronics.crt.consumer.generic", "does not exist"},
        {"a socket the prefab lacks", "furniture.display_case.generic", "runtime.sockets.glass"},
        {"no license", "furniture.wall_shelf.featured.generic", "source.license"},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        rv::test::LogCapture log;
        const ResolvedPrefab resolved = resolveOk(root.dir.path(), families, c.id);
        CHECK(resolved.source != PrefabSource::Requested);
        CHECK(resolved.fallbackReason.find(c.reason) != std::string::npos);
        CHECK(resolved.prefab.has_value());
        CHECK(log.count("warning") == 1);
    }
    // A family's generic falls back to the family's procedural prefab, of its own kind.
    const ResolvedPrefab bookends = resolveOk(root.dir.path(), families, "decor.bookends.generic");
    CHECK(bookends.source == PrefabSource::FamilyProcedural);
    CHECK(bookends.generator == "bookends");
    CHECK(resolveOk(root.dir.path(), families, "media.cartridge.home16.generic").source ==
          PrefabSource::FamilyProcedural);
    CHECK(rv::geometry::findSocket(
              resolveOk(root.dir.path(), families, "media.cartridge.home16.generic").prefab->sockets, "Spine") !=
          nullptr);
}

TEST_CASE("assets/resolver: ids without a family or of the wrong shape are errors", "[assets][resolver]") {
    const PrefabFamilies families = shippedFamilies();
    rv::test::LogCapture log;
    const auto unknown = rv::assets::resolvePrefab(kAssetRoot, families, "arcade.cabinet.upright");
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == ErrorCode::NotFound);
    for (const std::string_view bad : {"", "shelf", "../manifests/x", "Furniture.shelf", "a..b"}) {
        INFO(bad);
        const auto invalid = rv::assets::resolvePrefab(kAssetRoot, families, bad);
        REQUIRE_FALSE(invalid.has_value());
        CHECK(invalid.error().code == ErrorCode::InvalidArgument);
    }
    CHECK(log.count("warning") == 0); // the caller reports what it could not resolve
    // A variant that does not exist resolves to its family's generic prefab.
    const ResolvedPrefab variant = resolveOk(kAssetRoot, families, "electronics.crt.consumer.wide");
    CHECK(variant.source == PrefabSource::FamilyGeneric);
    CHECK(variant.manifest->id == "electronics.crt.consumer.generic");
    CHECK(rv::assets::toString(PrefabSource::FamilyProcedural) == "family procedural");
}
