// rv_library (ADR-0018 §4, §13): a library's draft and creation, the stage a data directory stands at, the preview against
// import --dry-run's counts, the folder listings over what vanished. The fixture is the snes-room collection.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <string>

#include <rv/catalog/ids.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/library/folder_listing.hpp>
#include <rv/library/library_creation.hpp>
#include <rv/library/library_preview.hpp>

#include "support/fixture.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::library::DraftProblem;
using rv::library::LibraryStage;

namespace {

rv::catalog::LibraryId defaultId() {
    return rv::catalog::LibraryId::parse(rv::catalog::LibraryId::kDefault).value();
}

rv::library::LibraryDraftRequest requestFor(const rv::test::Fixture& fixture, const fs::path& data) {
    rv::library::LibraryDraftRequest request;
    request.id = defaultId();
    request.homeFolder = fixture.root();
    request.installFolder = fixture.path("Programs/ES-DE");
    request.userData = data;
    return request;
}

rv::core::Paths pathsOf(const fs::path& data) {
    rv::core::PathOptions options;
    options.userDataOverride = data;
    rv::core::PathEnvironment environment;
    environment.executableDir = rv::test::testProgramsDir();
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return rv::core::Paths::resolve(options, environment).value();
}

} // namespace

TEST_CASE("library: a draft without a machine stands on the folders given", "[library]") {
    const auto fixture = rv::test::Fixture::materialize("esde/snes-room").value();
    const rv::test::TempDir data("rv-library");
    const rv::library::NoSystemProbe probe;
    const auto draft = rv::library::draftLibrary(requestFor(fixture, data.path()), probe);
    REQUIRE(draft.has_value());
    CHECK(draft->problem == DraftProblem::None);
    REQUIRE(draft->profile.has_value());
    CHECK(draft->profile->esdeHome == fixture.path("ES-DE"));
    REQUIRE(draft->roots.rom.has_value());
    CHECK(draft->roots.rom->path == fixture.path("roms"));
    CHECK_FALSE(draft->folders.empty());
}

TEST_CASE("library: what stops a draft is named, and nothing is written", "[library]") {
    const auto fixture = rv::test::Fixture::materialize("esde/snes-room").value();
    const rv::test::TempDir data("rv-library");
    const rv::library::NoSystemProbe probe;

    SECTION("no home anywhere") {
        auto request = requestFor(fixture, data.path());
        request.homeFolder.reset();
        const auto draft = rv::library::draftLibrary(request, probe);
        REQUIRE(draft.has_value());
        CHECK(draft->problem == DraftProblem::HomeUnknown);
    }
    SECTION("a home that is not there") {
        auto request = requestFor(fixture, data.path());
        request.homeFolder = data.path() / "nowhere";
        const auto draft = rv::library::draftLibrary(request, probe);
        REQUIRE(draft.has_value());
        CHECK(draft->problem == DraftProblem::HomeMissing);
    }
    SECTION("the data directory inside the library: the guard") {
        const auto draft = rv::library::draftLibrary(requestFor(fixture, fixture.path("roms/data")), probe);
        REQUIRE(draft.has_value());
        CHECK(draft->problem == DraftProblem::DataDirectoryInside);
        REQUIRE(draft->dataDirectory.has_value());
        CHECK(draft->dataDirectory->code == rv::ErrorCode::InvalidArgument);
        // A draft with a problem writes nothing.
        const fs::path file = data.path() / "library.json";
        const auto refused = rv::library::createLibrary(*draft, file, rv::esde::ExistingFile::Keep);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == rv::ErrorCode::InvalidArgument);
        CHECK_FALSE(fs::exists(file));
    }
}

TEST_CASE("library: createLibrary keeps a file it was not told to replace", "[library]") {
    const auto fixture = rv::test::Fixture::materialize("esde/snes-room").value();
    const rv::test::TempDir data("rv-library");
    const rv::library::NoSystemProbe probe;
    const auto draft = rv::library::draftLibrary(requestFor(fixture, data.path()), probe);
    REQUIRE(draft.has_value());
    const fs::path file = data.path() / "config" / "library.json";
    REQUIRE(rv::library::createLibrary(*draft, file, rv::esde::ExistingFile::Keep).has_value());
    const auto second = rv::library::createLibrary(*draft, file, rv::esde::ExistingFile::Keep);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == rv::ErrorCode::Conflict);
    CHECK(rv::esde::readLibraryFile(file)->id == defaultId());
}

TEST_CASE("library: the stage a data directory stands at comes from its files", "[library]") {
    const rv::test::TempDir data("rv-library");
    const rv::core::Paths paths = pathsOf(data.path());
    CHECK(rv::library::libraryStage(paths) == LibraryStage::NoLibrary);

    const auto fixture = rv::test::Fixture::materialize("esde/snes-room").value();
    const rv::library::NoSystemProbe probe;
    const auto draft = rv::library::draftLibrary(requestFor(fixture, data.path()), probe);
    REQUIRE(draft.has_value());
    REQUIRE(rv::library::createLibrary(*draft, paths.configDir() / rv::esde::kLibraryFileName,
                                       rv::esde::ExistingFile::Keep)
                .has_value());
    CHECK(rv::library::libraryStage(paths) == LibraryStage::NoCatalog);
    // Asking made nothing.
    CHECK_FALSE(fs::exists(paths.userDataRoot() / "database"));
}

TEST_CASE("library: the preview reads without writing and counts as the dry run", "[library]") {
    const auto fixture = rv::test::Fixture::materialize("esde/snes-room").value();
    const rv::test::TempDir data("rv-library");
    const rv::library::NoSystemProbe probe;
    const auto draft = rv::library::draftLibrary(requestFor(fixture, data.path()), probe);
    REQUIRE(draft.has_value());
    const auto preview = rv::library::previewLibrary(*draft->profile);
    REQUIRE(preview.has_value());
    CHECK(preview->total.entries == preview->snapshot.entries.size());
    CHECK(preview->total.games > 0);
    CHECK(preview->counts.contains("snes"));
    CHECK(preview->counts.at("snes").games == preview->total.games);
    const auto coverage = rv::library::previewCoverage(*preview);
    REQUIRE(coverage.has_value());
    CHECK(coverage->entries == preview->total.games);
}

TEST_CASE("library: a preview cancelled ends with Cancelled", "[library]") {
    const auto fixture = rv::test::Fixture::materialize("esde/snes-room").value();
    const rv::test::TempDir data("rv-library");
    const rv::library::NoSystemProbe probe;
    const auto draft = rv::library::draftLibrary(requestFor(fixture, data.path()), probe);
    REQUIRE(draft.has_value());
    std::atomic<bool> cancel{true};
    rv::library::PreviewOptions options;
    options.cancel = &cancel;
    const auto preview = rv::library::previewLibrary(*draft->profile, options);
    REQUIRE_FALSE(preview.has_value());
    CHECK(preview.error().code == rv::ErrorCode::Cancelled);
}

TEST_CASE("library: folders are listed by name, a vanished one says so", "[library]") {
    const rv::test::TempDir data("rv-library");
    fs::create_directories(data.path() / "Zeta");
    fs::create_directories(data.path() / "alpha");
    fs::create_directories(data.path() / "$RECYCLE.BIN");
    REQUIRE(rv::core::writeTextFileAtomic(data.path() / "a file.txt", "x").has_value());
    const auto folders = rv::library::listFolders(data.path());
    REQUIRE(folders.has_value());
    REQUIRE(folders->size() == 2);
    CHECK((*folders)[0].name == "alpha");
    CHECK((*folders)[1].name == "Zeta");

    const fs::path gone = data.path() / "gone";
    const auto vanished = rv::library::listFolders(gone);
    REQUIRE_FALSE(vanished.has_value());
    CHECK(vanished.error().code == rv::ErrorCode::NotFound);
    CHECK(rv::library::checkAccess(gone).availability != rv::catalog::Availability::Available);
    CHECK(rv::library::checkAccess(data.path()).availability == rv::catalog::Availability::Available);
    CHECK_FALSE(rv::library::listDrives().empty());
}
