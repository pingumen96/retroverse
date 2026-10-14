#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "support/temp_dir.hpp"
#include <rv/core/paths.hpp>

namespace fs = std::filesystem;
using rv::core::AssetRootSource;
using rv::core::PathEnvironment;
using rv::core::PathOptions;
using rv::core::Paths;

namespace {

struct Layout {
    rv::test::TempDir root{"rv-paths"};
    fs::path exeDir = root.file("bin");
    fs::path exeAssets = root.file("bin/assets");
    fs::path sourceAssets = root.file("source/assets");
    fs::path envAssets = root.file("env-assets");
    fs::path cliAssets = root.file("cli-assets");
    fs::path localAppData = root.file("LocalAppData");

    Layout() {
        for (const fs::path& dir : {exeAssets, sourceAssets, envAssets, cliAssets, localAppData}) {
            fs::create_directories(dir);
        }
    }

    [[nodiscard]] PathEnvironment environment() const {
        PathEnvironment env;
        env.executableDir = exeDir;
        env.localAppData = localAppData;
        env.sourceAssetDir = sourceAssets;
        return env;
    }
};

} // namespace

TEST_CASE("core/paths: asset root precedence", "[core][paths]") {
    Layout layout;
    PathEnvironment env = layout.environment();
    PathOptions options;

    SECTION("source tree beats the executable directory in dev builds") {
        const auto paths = Paths::resolve(options, env);
        REQUIRE(paths.has_value());
        CHECK(paths->assetRootSource() == AssetRootSource::SourceTree);
        CHECK(fs::equivalent(paths->assetRoot(), layout.sourceAssets));
    }
    SECTION("executable directory when no source tree is compiled in") {
        env.sourceAssetDir.reset();
        const auto paths = Paths::resolve(options, env);
        REQUIRE(paths.has_value());
        CHECK(paths->assetRootSource() == AssetRootSource::ExecutableDir);
        CHECK(fs::equivalent(paths->assetRoot(), layout.exeAssets));
    }
    SECTION("environment variable beats the source tree") {
        env.envAssetRoot = layout.envAssets;
        const auto paths = Paths::resolve(options, env);
        REQUIRE(paths.has_value());
        CHECK(paths->assetRootSource() == AssetRootSource::Environment);
    }
    SECTION("command line beats everything") {
        env.envAssetRoot = layout.envAssets;
        options.assetRootOverride = layout.cliAssets;
        const auto paths = Paths::resolve(options, env);
        REQUIRE(paths.has_value());
        CHECK(paths->assetRootSource() == AssetRootSource::CommandLine);
        CHECK(fs::equivalent(paths->assetRoot(), layout.cliAssets));
    }
    SECTION("explicit choices must exist") {
        options.assetRootOverride = layout.root.file("missing");
        const auto paths = Paths::resolve(options, env);
        REQUIRE_FALSE(paths.has_value());
        CHECK(paths.error().code == rv::ErrorCode::NotFound);
    }
    SECTION("no candidate at all is an error") {
        env.sourceAssetDir.reset();
        env.executableDir = layout.root.file("elsewhere");
        const auto paths = Paths::resolve(options, env);
        REQUIRE_FALSE(paths.has_value());
        CHECK(paths.error().code == rv::ErrorCode::NotFound);
    }
}

TEST_CASE("core/paths: user data root", "[core][paths]") {
    Layout layout;
    PathEnvironment env = layout.environment();
    PathOptions options;

    SECTION("installed mode uses LOCALAPPDATA/Retroverse") {
        const auto paths = Paths::resolve(options, env);
        REQUIRE(paths.has_value());
        CHECK(paths->userDataRoot() == (layout.localAppData / "Retroverse").lexically_normal());
        CHECK(paths->logsDir() == paths->userDataRoot() / "logs");
        CHECK_FALSE(paths->isPortable());
    }
    SECTION("portable mode keeps data next to the executable") {
        options.portable = true;
        const auto paths = Paths::resolve(options, env);
        REQUIRE(paths.has_value());
        CHECK(paths->userDataRoot().filename() == "RetroverseData");
        CHECK(paths->isPortable());
    }
    SECTION("explicit data directory") {
        options.userDataOverride = layout.root.file("custom-data");
        const auto paths = Paths::resolve(options, env);
        REQUIRE(paths.has_value());
        CHECK(paths->userDataRoot().filename() == "custom-data");
    }
}

TEST_CASE("core/paths: asset-relative paths are confined to the asset root", "[core][paths]") {
    Layout layout;
    const auto paths = Paths::resolve(PathOptions{}, layout.environment());
    REQUIRE(paths.has_value());

    const auto shader = paths->asset("shaders/pbr/pbr_base.frag");
    REQUIRE(shader.has_value());
    CHECK(*shader == paths->assetRoot() / "shaders" / "pbr" / "pbr_base.frag");

    const auto backslashes = paths->asset("shaders\\common\\common.glsl");
    REQUIRE(backslashes.has_value());
    CHECK(backslashes->filename() == "common.glsl");

    CHECK_FALSE(paths->asset("../secrets.txt").has_value());
    CHECK_FALSE(paths->asset("shaders/../../escape.txt").has_value());
    CHECK_FALSE(paths->asset("C:/Windows/win.ini").has_value());
    CHECK_FALSE(paths->asset("").has_value());
}
