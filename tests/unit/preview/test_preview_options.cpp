#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string>
#include <vector>

#include <rv/core/text.hpp>
#include <rv/preview/preview_options.hpp>

using rv::ErrorCode;
using rv::preview::PreviewOptions;
using rv::preview::PreviewTarget;

namespace {

rv::Expected<PreviewOptions> parse(std::initializer_list<const char*> arguments) {
    std::vector<std::string> list;
    for (const char* argument : arguments) {
        list.emplace_back(argument);
    }
    return rv::preview::parsePreviewCommandLine(list);
}

} // namespace

TEST_CASE("preview/options: targets are models, manifests or procedural prefabs", "[preview][options]") {
    const auto model = rv::preview::parsePreviewTarget("build/preview/crt_fixture.glb");
    REQUIRE(model.has_value());
    CHECK(model->kind == PreviewTarget::Kind::Model);
    CHECK(model->label() == "build/preview/crt_fixture.glb");
    CHECK(rv::preview::parsePreviewTarget("C:\\Models\\Lamp.GLTF")->kind == PreviewTarget::Kind::Model);
    // Shown with one kind of separator, as a dropped file's native path would not be.
    CHECK(rv::preview::parsePreviewTarget("C:\\Models\\Lamp.GLTF")->label() == "C:/Models/Lamp.GLTF");
    CHECK(rv::preview::parsePreviewTarget("assets/manifests/prefabs/x.json")->kind == PreviewTarget::Kind::Manifest);

    const auto prefab = rv::preview::parsePreviewTarget("prefab:crt");
    REQUIRE(prefab.has_value());
    CHECK(prefab->kind == PreviewTarget::Kind::Prefab);
    CHECK(prefab->prefabKind == "crt");
    CHECK(prefab->params.is_object());
    CHECK(prefab->params.empty());
    CHECK(prefab->label() == "prefab:crt");

    for (const char* bad : {"", "prefab:television", "prefab:", "model.obj", "notes.txt", "model"}) {
        INFO(bad);
        const auto refused = rv::preview::parsePreviewTarget(bad);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("preview/options: the command line takes a target, prefab parameters and the host options",
          "[preview][options]") {
    const auto options = parse({"prefab:shelf", "--params", R"({"width": 0.8, "levels": 3})", "--exit-after-frames",
                                "--frames", "5", "--hidden", "--screenshot", "out.png", "--data-dir", "build/rvdata"});
    REQUIRE(options.has_value());
    REQUIRE(options->target.has_value());
    CHECK(options->target->prefabKind == "shelf");
    CHECK(options->target->params.at("levels") == 3);
    CHECK(options->run.frameLimit == std::optional<std::uint32_t>(5));
    CHECK(options->run.hidden);
    CHECK(options->run.noDialogs); // implied by the frame limit
    CHECK(options->run.screenshot == std::optional<std::filesystem::path>(rv::core::pathFromUtf8("out.png")));
    CHECK(options->run.dataDir.has_value());
    // The panels are the tool: shown from the start and in screenshots.
    CHECK(options->run.devUiOpen);
    CHECK(options->run.screenshotIncludeUi);
    CHECK_FALSE(options->writeFixture.has_value());

    // No target: an empty preview, waiting for a dropped file.
    const auto empty = parse({"--smoke"});
    REQUIRE(empty.has_value());
    CHECK_FALSE(empty->target.has_value());
    CHECK(empty->run.smoke);
}

TEST_CASE("preview/options: --view picks the view mode at start", "[preview][options]") {
    CHECK(parse({"model.glb"})->view == rv::preview::ViewMode::Lit);
    CHECK(parse({"model.glb", "--view", "uv-checker"})->view == rv::preview::ViewMode::UvChecker);
    for (const rv::preview::ViewMode mode : rv::preview::kViewModes) {
        CHECK(rv::preview::viewModeFromName(rv::preview::viewModeName(mode)) == mode);
    }
    const auto unknown = parse({"model.glb", "--view", "xray"});
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().context == "--view=xray");

    // --panel brings a tab forward at start.
    const auto materials = parse({"model.glb", "--panel", "Materials"});
    REQUIRE(materials.has_value());
    CHECK(materials->panel == std::optional<std::string>("Materials"));
    CHECK(parse({"model.glb", "--panel", ""}).error().context == "--panel");
}

TEST_CASE("preview/options: --write-fixture writes and exits, alone", "[preview][options]") {
    const auto fixture = parse({"--write-fixture", "build/preview/crt_fixture.glb"});
    REQUIRE(fixture.has_value());
    REQUIRE(fixture->writeFixture.has_value());
    CHECK(rv::core::pathToUtf8(*fixture->writeFixture) == "build/preview/crt_fixture.glb");
    CHECK_FALSE(fixture->target.has_value());

    CHECK(parse({"--write-fixture", "fixture.gltf"}).error().code == ErrorCode::InvalidArgument);
    CHECK(parse({"model.glb", "--write-fixture", "fixture.glb"}).error().code == ErrorCode::InvalidArgument);
    CHECK(parse({"--write-fixture", "fixture.glb", "--smoke"}).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("preview/options: bad arguments name the option", "[preview][options]") {
    // --params: a JSON object, for prefabs only.
    const auto notObject = parse({"prefab:crt", "--params", "[1, 2]"});
    REQUIRE_FALSE(notObject.has_value());
    CHECK(notObject.error().context.find("--params") != std::string::npos);
    CHECK(parse({"prefab:crt", "--params", "{width: }"}).error().code == ErrorCode::ParseError);
    CHECK(parse({"model.glb", "--params", "{}"}).error().code == ErrorCode::InvalidArgument);
    CHECK(parse({"--params", "{}"}).error().code == ErrorCode::InvalidArgument);
    // One target; the app's own options are not the tool's; host rules apply.
    CHECK(parse({"a.glb", "b.glb"}).error().code == ErrorCode::InvalidArgument);
    CHECK(parse({"a.glb", "--scene", "m0_testroom"}).error().code == ErrorCode::InvalidArgument);
    CHECK(parse({"a.glb", "--screenshot", "out.png"}).error().context == "--screenshot");
    CHECK(parse({"a.blend"}).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("preview/options: help lists the targets, the controls, the options and the exit codes",
          "[preview][options]") {
    const auto help = parse({"--help"});
    REQUIRE(help.has_value());
    CHECK(help->run.showHelp);
    const std::string text = rv::preview::previewHelp();
    for (const char* needle : {"prefab:<kind>", "--params", "--write-fixture", "--screenshot-size", "--smoke",
                               "--exit-after-frames", "--data-dir", "Exit codes", "left drag orbits"}) {
        INFO(needle);
        CHECK(text.find(needle) != std::string::npos);
    }
}
