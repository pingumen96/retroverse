#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "support/fixture_console.hpp"
#include "support/temp_dir.hpp"
#include <rv/assets/console_content.hpp>
#include <rv/preview/preview_options.hpp>
#include <rv/assets/glb_writer.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/preview/crt_fixture.hpp>
#include <rv/preview/preview_subject.hpp>

using rv::assets::ValidationIssue;
using rv::preview::PreviewMessage;
using rv::preview::PreviewSubject;
using rv::preview::PreviewTarget;

namespace {

const std::filesystem::path kAssetRoot = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);

/// A manifest of the fixture whose model is `model` and whose screen socket is `screen`.
std::string fixtureManifest(std::string_view model, std::string_view screen) {
    return std::string(R"({
  "schemaVersion": 1,
  "id": "electronics.crt.fixture",
  "assetType": "electronic_screen",
  "category": "crt",
  "role": "hero",
  "source": {"kind": "original", "url": null, "author": "Retroverse tests", "acquiredAt": "2026-10-15",
             "license": "MIT", "licenseSnapshot": null, "attributionRequired": false, "redistributionApproved": true},
  "runtime": {
    "model": ")") +
           std::string(model) + R"(",
    "scaleMeters": [0.71, 0.61, 0.56],
    "sockets": {"screen": ")" +
           std::string(screen) + R"(", "inspect": "Anchor_ScreenLook", "stand": "TV_Stand"},
    "collision": "proxy",
    "videoSurface": {"aspectRatio": "4:3", "meshNode": "ScreenSurface"}
  },
  "tags": ["crt"],
  "fallbackPrefab": "electronics.crt.consumer.generic",
  "hash": "sha256:TO_BE_GENERATED_BY_BUILD"
})";
}

PreviewTarget target(std::string_view text) {
    auto parsed = rv::preview::parsePreviewTarget(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

PreviewTarget fileTarget(const std::filesystem::path& file) {
    return target(rv::core::pathToUtf8(file));
}

bool hasMessage(const PreviewSubject& subject, PreviewMessage::Severity severity, std::string_view needle) {
    return std::any_of(subject.messages.begin(), subject.messages.end(), [&](const PreviewMessage& message) {
        return message.severity == severity && message.text.find(needle) != std::string::npos;
    });
}

std::size_t issues(const PreviewSubject& subject, ValidationIssue::Severity severity) {
    REQUIRE(subject.manifest.has_value());
    return subject.manifest->count(severity);
}

} // namespace

TEST_CASE("preview/subject: a model loads with its sockets and the manifest that sits next to it",
          "[preview][subject]") {
    rv::test::TempDir dir;
    const auto model = dir.file("crt_fixture.glb");
    REQUIRE(rv::assets::writeGlb(model, rv::preview::makeCrtFixture()).has_value());

    const PreviewSubject alone = rv::preview::loadPreviewSubject(fileTarget(model), kAssetRoot);
    REQUIRE(alone.model != nullptr);
    CHECK(alone.title == "crt_fixture.glb");
    CHECK_FALSE(alone.placeholder);
    CHECK(alone.sockets.size() == 4);
    CHECK(rv::core::nearlyEqual(alone.bounds.size(), rv::preview::kCrtFixtureSize, 1e-5f));
    CHECK_FALSE(alone.manifest.has_value());
    // The model and both places a manifest could appear are watched.
    CHECK(alone.watchedFiles.size() == 3);
    CHECK(alone.count(PreviewMessage::Severity::Error) == 0);

    // A manifest next to the model is checked against its sockets and size: only the placeholder hash warns.
    REQUIRE(rv::core::writeTextFileAtomic(dir.file("crt_fixture.manifest.json"),
                                          fixtureManifest("crt_fixture.glb", "ScreenSurface"))
                .has_value());
    const PreviewSubject checked = rv::preview::loadPreviewSubject(fileTarget(model), kAssetRoot);
    REQUIRE(checked.manifest.has_value());
    REQUIRE(checked.manifest->manifest.has_value());
    CHECK(checked.manifest->manifest->id == "electronics.crt.fixture");
    CHECK(issues(checked, ValidationIssue::Severity::Error) == 0);
    CHECK(issues(checked, ValidationIssue::Severity::Warning) == 1);
    CHECK(hasMessage(checked, PreviewMessage::Severity::Warning, "0 error(s), 1 warning(s)"));
    // The summary is about the manifest: the asset itself loaded cleanly.
    CHECK(checked.count(PreviewMessage::Severity::Warning, PreviewMessage::Topic::Manifest) == 1);
    CHECK(checked.count(PreviewMessage::Severity::Warning, PreviewMessage::Topic::Asset) == 0);
}

TEST_CASE("preview/subject: a .gltf model is watched with its buffer and its images", "[preview][subject]") {
    rv::test::TempDir dir;
    rv::assets::GlbDescription description = rv::preview::makeCrtFixture();
    description.patchJson = [](nlohmann::json& json) {
        json["images"][0] = {{"uri", "textures/plastic.png"}};
    };
    auto files = rv::assets::buildGltfWithExternalBuffer(description, "crt_fixture.bin");
    REQUIRE(files.has_value());
    REQUIRE(rv::core::writeTextFileAtomic(dir.file("crt_fixture.gltf"), files->json).has_value());
    REQUIRE(rv::core::writeFileAtomic(dir.file("crt_fixture.bin"), files->bin).has_value());

    // The image is missing: its texture is dropped with a warning, and its file is watched, so adding it reloads.
    const PreviewSubject subject =
        rv::preview::loadPreviewSubject(fileTarget(dir.file("crt_fixture.gltf")), kAssetRoot);
    REQUIRE(subject.model != nullptr);
    CHECK(subject.count(PreviewMessage::Severity::Warning) > 0);
    const auto watched = [&subject](const std::filesystem::path& file) {
        return std::any_of(subject.watchedFiles.begin(), subject.watchedFiles.end(),
                           [&file](const std::filesystem::path& path) { return path == file; });
    };
    CHECK(watched(dir.file("crt_fixture.gltf")));
    CHECK(watched(dir.file("crt_fixture.bin")));
    CHECK(watched(dir.file("textures/plastic.png")));
    CHECK(watched(dir.file("crt_fixture.manifest.json")));
    CHECK(subject.watchedFiles.size() == 5);
}

TEST_CASE("preview/subject: the Manifest panel comes forward for a manifest not shown before", "[preview][subject]") {
    PreviewSubject plain;
    PreviewSubject withManifest;
    withManifest.manifest.emplace();
    // A new target with a manifest; none without.
    CHECK(rv::preview::bringsManifestForward(plain, withManifest, false));
    CHECK(rv::preview::bringsManifestForward(withManifest, withManifest, false));
    CHECK_FALSE(rv::preview::bringsManifestForward(withManifest, plain, false));
    // Reloads: a manifest just created next to the model comes forward, one already shown leaves the tabs alone.
    CHECK(rv::preview::bringsManifestForward(plain, withManifest, true));
    CHECK_FALSE(rv::preview::bringsManifestForward(withManifest, withManifest, true));
    CHECK_FALSE(rv::preview::bringsManifestForward(plain, plain, true));
}

TEST_CASE("preview/subject: a manifest with a broken socket name reports the error", "[preview][subject]") {
    rv::test::TempDir dir;
    REQUIRE(rv::assets::writeGlb(dir.file("crt_fixture.glb"), rv::preview::makeCrtFixture()).has_value());
    // The model is not under the asset root: it is found next to the manifest.
    const auto manifest = dir.file("broken.json");
    REQUIRE(rv::core::writeTextFileAtomic(manifest, fixtureManifest("crt_fixture.glb", "ScreenSurfaceX")).has_value());

    const PreviewSubject subject = rv::preview::loadPreviewSubject(fileTarget(manifest), kAssetRoot);
    CHECK(subject.title == "electronics.crt.fixture");
    REQUIRE(subject.model != nullptr);
    CHECK(subject.modelFile.filename() == "crt_fixture.glb");
    CHECK_FALSE(subject.placeholder);
    REQUIRE(subject.manifest.has_value());
    const auto& found = subject.manifest->issues;
    CHECK(std::any_of(found.begin(), found.end(), [](const ValidationIssue& issue) {
        return issue.severity == ValidationIssue::Severity::Error &&
               issue.message == "runtime.sockets.screen: socket \"ScreenSurfaceX\" does not exist";
    }));
    CHECK(hasMessage(subject, PreviewMessage::Severity::Error, "1 error(s)"));
    // The manifest and its model are watched.
    CHECK(subject.watchedFiles.size() == 2);
}

TEST_CASE("preview/subject: the CRT manifest builds its procedural prefab", "[preview][subject]") {
    const auto manifest = kAssetRoot / "manifests/prefabs/electronics.crt.consumer.generic.json";
    const PreviewSubject subject = rv::preview::loadPreviewSubject(fileTarget(manifest), kAssetRoot);
    CHECK(subject.title == "electronics.crt.consumer.generic");
    REQUIRE(subject.prefab.has_value());
    CHECK(subject.prefabKind == "crt");
    CHECK(subject.model == nullptr);
    CHECK_FALSE(subject.placeholder);
    CHECK(rv::core::nearlyEqual(subject.bounds.size(), glm::vec3(0.71f, 0.61f, 0.56f), 1e-4f));
    // Every socket it names exists; the hash is still the placeholder.
    CHECK(issues(subject, ValidationIssue::Severity::Error) == 0);
    CHECK(issues(subject, ValidationIssue::Severity::Warning) == 1);
}

TEST_CASE("preview/subject: what cannot be loaded becomes a message and the placeholder", "[preview][subject]") {
    rv::test::TempDir dir;
    // A missing model file.
    const PreviewSubject missing = rv::preview::loadPreviewSubject(fileTarget(dir.file("missing.glb")), kAssetRoot);
    CHECK(missing.placeholder);
    CHECK(missing.model == nullptr);
    CHECK(hasMessage(missing, PreviewMessage::Severity::Error, "cannot be loaded"));
    CHECK(rv::core::nearlyEqual(missing.bounds.size(), glm::vec3(0.5f)));

    // Prefab parameters out of range name the parameter.
    PreviewTarget crt = target("prefab:crt");
    crt.params = {{"width", 50.0}};
    const PreviewSubject refused = rv::preview::loadPreviewSubject(crt, kAssetRoot);
    CHECK(refused.placeholder);
    CHECK_FALSE(refused.prefab.has_value());
    CHECK(refused.prefabKind == "crt");
    CHECK(hasMessage(refused, PreviewMessage::Severity::Error, "crt.width"));

    // A manifest whose model is nowhere, and one that is not even JSON.
    const auto lost = dir.file("lost.json");
    REQUIRE(rv::core::writeTextFileAtomic(lost, fixtureManifest("models/nowhere.glb", "ScreenSurface")).has_value());
    const PreviewSubject nowhere = rv::preview::loadPreviewSubject(fileTarget(lost), kAssetRoot);
    CHECK(nowhere.placeholder);
    CHECK(hasMessage(nowhere, PreviewMessage::Severity::Error, "models/nowhere.glb"));
    REQUIRE(nowhere.manifest.has_value());
    CHECK(nowhere.manifest->manifest.has_value());

    const auto garbage = dir.file("garbage.json");
    REQUIRE(rv::core::writeTextFileAtomic(garbage, "{ not json").has_value());
    const PreviewSubject broken = rv::preview::loadPreviewSubject(fileTarget(garbage), kAssetRoot);
    CHECK(broken.placeholder);
    REQUIRE(broken.manifest.has_value());
    CHECK_FALSE(broken.manifest->manifest.has_value());
    CHECK(broken.manifest->error.has_value());
}

TEST_CASE("preview/subject: a manifest's model path never leaves its directories", "[preview][subject]") {
    rv::test::TempDir dir;
    REQUIRE(rv::assets::writeGlb(dir.file("crt_fixture.glb"), rv::preview::makeCrtFixture()).has_value());
    const auto manifest = dir.file("nested/manifest.json");
    CHECK(rv::preview::resolveManifestModel("../crt_fixture.glb", manifest, kAssetRoot) == std::nullopt);
    CHECK(rv::preview::resolveManifestModel(rv::core::pathToUtf8(dir.file("crt_fixture.glb")), manifest,
                                            kAssetRoot) == std::nullopt); // absolute
    CHECK(rv::preview::resolveManifestModel("crt_fixture.glb", dir.file("manifest.json"), kAssetRoot).has_value());
    const auto sidecars = rv::preview::sidecarManifestPaths(dir.file("crt_fixture.glb"));
    CHECK(sidecars[0].filename() == "crt_fixture.manifest.json");
    CHECK(sidecars[1].filename() == "crt_fixture.json");
}

TEST_CASE("preview/subject: a nominal size more than 5% off the measured bounds warns", "[preview][subject]") {
    rv::assets::PrefabManifest manifest;
    manifest.runtime.scaleMeters = glm::vec3(0.71f, 0.61f, 0.56f);
    const auto measured = [](glm::vec3 size) {
        return rv::core::Aabb::fromMinMax(glm::vec3(0.0f), size);
    };
    CHECK_FALSE(rv::preview::scaleIssue(manifest, measured(glm::vec3(0.72f, 0.6f, 0.57f))).has_value());
    const auto centimeters = rv::preview::scaleIssue(manifest, measured(glm::vec3(71.0f, 61.0f, 56.0f)));
    REQUIRE(centimeters.has_value());
    CHECK(centimeters->severity == ValidationIssue::Severity::Warning);
    CHECK(centimeters->message.starts_with("runtime.scaleMeters: 0.710 x 0.610 x 0.560 m"));
    const auto deeper = rv::preview::scaleIssue(manifest, measured(glm::vec3(0.71f, 0.61f, 0.62f)));
    REQUIRE(deeper.has_value());
    CHECK(deeper->message.find("z differs by 11%") != std::string::npos);
    manifest.runtime.scaleMeters.reset();
    CHECK_FALSE(rv::preview::scaleIssue(manifest, measured(glm::vec3(5.0f))).has_value());
}

TEST_CASE("preview/subject: a console's content folder: its model, console.json checked as the app checks it (I11 "
          "step 7)",
          "[preview][console_content]") {
    const rv::test::TempDir dir;
    const std::filesystem::path folder = dir.path() / "content" / "consoles" / "snes";
    REQUIRE(rv::test::writeFixtureConsole(folder / "console.glb").has_value());
    REQUIRE(rv::core::writeTextFileAtomic(folder / "console.json", rv::assets::writeConsoleManifest({})).has_value());
    auto target = rv::preview::parsePreviewTarget(rv::core::pathToUtf8(folder));
    REQUIRE(target.has_value());
    CHECK(target->kind == rv::preview::PreviewTarget::Kind::Model);
    CHECK(target->file == folder / "console.glb");
    const rv::preview::PreviewSubject subject =
        rv::preview::loadPreviewSubject(*target, rv::core::pathFromUtf8(RV_TEST_ASSET_DIR));
    REQUIRE(subject.model != nullptr);
    CHECK_FALSE(subject.manifest.has_value()); // console.json is no prefab manifest
    const auto console = std::find_if(subject.messages.begin(), subject.messages.end(),
                                      [](const rv::preview::PreviewMessage& message) {
                                          return message.text.starts_with("Console (home): shown in its room");
                                      });
    CHECK(console != subject.messages.end());
    CHECK(std::find(subject.watchedFiles.begin(), subject.watchedFiles.end(), folder / "console.json") !=
          subject.watchedFiles.end());
}
