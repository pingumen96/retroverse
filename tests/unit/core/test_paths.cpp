#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <vector>

#include "support/file_system.hpp"
#include "support/temp_dir.hpp"
#include <rv/core/paths.hpp>
#include <rv/core/utf8.hpp>

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
        CHECK(paths->databaseDir() == paths->userDataRoot() / "database");
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
    // Malformed UTF-8 is an error, never an exception.
    const auto malformed = paths->asset("shaders/\xC3.glsl");
    REQUIRE_FALSE(malformed.has_value());
    CHECK(malformed.error().code == rv::ErrorCode::InvalidArgument);
}

TEST_CASE("core/paths: one path within another, as the file system compares", "[core][paths]") {
    using rv::core::pathIsWithin;
    const fs::path home{L"C:\\Users\\X\\ES-DE"};
    CHECK(pathIsWithin(home, home));
    CHECK(pathIsWithin(fs::path(L"C:\\Users\\X\\ES-DE\\gamelists\\snes"), home));
    CHECK(pathIsWithin(fs::path(L"c:/users/x/es-de/Settings"), home)); // case and separators do not matter
    CHECK(pathIsWithin(fs::path(L"C:\\Users\\X\\ES-DE\\"), home));
    CHECK(pathIsWithin(fs::path(L"C:\\Users\\X\\Other\\..\\ES-DE\\a"), home)); // normalized first
    CHECK(pathIsWithin(fs::path(L"\\\\?\\C:\\Users\\X\\ES-DE\\a"), home));      // the \\?\ form
    CHECK(pathIsWithin(home, fs::path(L"C:\\")));
    CHECK_FALSE(pathIsWithin(fs::path(L"C:\\Users\\X\\ES-DE2"), home)); // a name that starts alike
    CHECK_FALSE(pathIsWithin(fs::path(L"C:\\Users\\X"), home));
    CHECK_FALSE(pathIsWithin(fs::path(L"D:\\Users\\X\\ES-DE"), home));
    CHECK_FALSE(pathIsWithin(fs::path(L"\\\\server\\share\\Users\\X\\ES-DE"), home));
    CHECK(pathIsWithin(fs::path(L"\\\\server\\share\\roms\\gb"), fs::path(L"\\\\SERVER\\Share\\roms")));
    // Letters with diacritics fold too; NFC and NFD names are different names.
    CHECK(pathIsWithin(fs::path(L"E:\\Gi\u00F2chi\\a"), fs::path(L"e:\\GI\u00D2CHI")));
    CHECK_FALSE(pathIsWithin(fs::path(L"E:\\Gio\u0300chi\\a"), fs::path(L"e:\\GI\u00D2CHI")));
    // Relative paths are taken against the current directory.
    CHECK(pathIsWithin(fs::path("a/b"), fs::current_path()));
    CHECK_FALSE(pathIsWithin(fs::current_path(), fs::path("a")));
}

TEST_CASE("core/paths: the user data directory never lies in what Retroverse only reads", "[core][paths]") {
    using rv::core::checkUserDataDirectory;
    rv::test::TempDir dir("rv-guard");
    const fs::path home = dir.file("ES-DE");
    const fs::path roms = dir.file("Roms");
    fs::create_directories(home);
    fs::create_directories(roms);
    const std::vector<fs::path> protectedRoots{home, fs::path(), roms, dir.file("Media (not there)")};
    const auto refusedIn = [&](const fs::path& userData) {
        const auto checked = checkUserDataDirectory(userData, protectedRoots);
        INFO(rv::core::pathToUtf8Lossy(userData));
        REQUIRE_FALSE(checked.has_value());
        CHECK(checked.error().code == rv::ErrorCode::InvalidArgument);
        return checked.error().message;
    };

    // Beside them, existing or not: allowed.
    CHECK(checkUserDataDirectory(dir.file("Retroverse"), protectedRoots).has_value());
    CHECK(checkUserDataDirectory(dir.file("ES-DE2/Retroverse"), protectedRoots).has_value());
    CHECK(checkUserDataDirectory(dir.path(), protectedRoots).has_value()); // around them is not inside
    CHECK(checkUserDataDirectory(dir.file("Retroverse"), {}).has_value());
    // Inside one of them, as written (any case, the root itself, a folder that does not exist yet).
    CHECK(refusedIn(home / "Retroverse").find("ES-DE") != std::string::npos);
    CHECK(refusedIn(home).find("ES-DE") != std::string::npos);
    CHECK(refusedIn(dir.file("roms/sub/data")).find("Roms") != std::string::npos);
    refusedIn(dir.file("Media (not there)/Retroverse"));

    // Through a junction: the path names another folder, the file system reaches the ES-DE home.
    auto toHome = rv::test::Junction::create(dir.file("Shortcut"), home);
    REQUIRE(toHome.has_value());
    CHECK(refusedIn(dir.file("Shortcut/Retroverse")).find("ES-DE") != std::string::npos);
    CHECK(refusedIn(dir.file("Shortcut")).find("ES-DE") != std::string::npos);
    // And a protected root named through a junction protects the folder it leads to.
    auto toRoms = rv::test::Junction::create(dir.file("RomsLink"), roms);
    REQUIRE(toRoms.has_value());
    const std::vector<fs::path> throughLink{dir.file("RomsLink")};
    CHECK_FALSE(checkUserDataDirectory(roms / "Retroverse", throughLink).has_value());
    CHECK(checkUserDataDirectory(home / "Retroverse", throughLink).has_value());
    // Nothing was created.
    CHECK_FALSE(fs::exists(dir.file("Retroverse")));
    CHECK_FALSE(fs::exists(home / "Retroverse"));
}
