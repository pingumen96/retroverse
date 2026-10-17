// The real library's media tree through rv::esde::EsdeMediaResolver (I2 step 8, ADR-0008 §10). Hidden
// ([.real-library]): ctest never runs it; scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the
// library before and after, and the folder in an environment variable:
//   RV_REAL_MEDIA_ROOT  ES-DE's MediaDirectory: every system folder under it is listed (no file is opened)
// The expected figures are the reference library's, from the survey's listing of 2026-10-16 (esde-data.md section 5:
// 44 system folders, 364,810 files, 11 kinds, the png and jpg counts, four .flv videos); a library scraped since
// changes them, and the test prints what it listed. It also prints the time the listing took: the snapshot the script
// takes just before has listed the same tree, so the time is a warm one (informative: the gate times the import).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include <psapi.h>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/media.hpp>
#include <rv/core/text.hpp>
#include <rv/core/tree_walk.hpp>
#include <rv/esde/media_resolver.hpp>

namespace fs = std::filesystem;
using rv::catalog::MediaKind;
using rv::esde::MediaFolderState;

namespace {

std::optional<fs::path> pathVariable(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return fs::path(value);
}

/// The process's private bytes (its commit charge): what the listings cost, informative.
std::uint64_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) {
        return 0;
    }
    return counters.PrivateUsage;
}

/// The names of the folders right under `root`, in the walker's order.
std::vector<std::string> foldersUnder(const fs::path& root) {
    std::vector<std::string> folders;
    const auto walked = rv::core::walkTree(root, [&](const rv::core::TreeEntry& entry) {
        if (entry.kind == rv::core::TreeEntryKind::Directory) {
            folders.push_back(entry.relativePath);
            return rv::core::WalkAction::SkipChildren;
        }
        return rv::core::WalkAction::Continue;
    });
    REQUIRE(walked.has_value());
    return folders;
}

} // namespace

TEST_CASE("esde/media: the real library's media tree", "[esde][media][.real-library]") {
    const auto root = pathVariable(L"RV_REAL_MEDIA_ROOT");
    if (!root) {
        SKIP("set RV_REAL_MEDIA_ROOT (scripts/real-library.ps1 does)");
    }
    const std::vector<std::string> folders = foldersUnder(*root);

    const std::uint64_t memoryBefore = privateBytes();
    const auto start = std::chrono::steady_clock::now();
    rv::esde::EsdeMediaResolver resolver(*root);
    REQUIRE(resolver.checkRoot().has_value());
    std::map<MediaKind, std::uint64_t> media;
    std::map<MediaKind, std::uint64_t> shadowed;
    std::map<std::string, std::uint64_t> ignored; // "<system>/<kind> <extension>"
    std::uint64_t unlisted = 0;
    std::uint64_t unreadable = 0;
    std::uint64_t listedSystems = 0;
    for (const std::string& folder : folders) {
        const auto system = rv::catalog::SystemName::parse(folder);
        REQUIRE(system.has_value());
        const auto listing = resolver.listing(*system);
        REQUIRE(listing.has_value());
        const rv::esde::SystemMediaListing& systemListing = **listing;
        CHECK(systemListing.state == MediaFolderState::Listed);
        CHECK(systemListing.folderName == folder); // ES-DE's names: the system folders are spelled as the systems
        listedSystems += systemListing.state == MediaFolderState::Listed ? 1 : 0;
        for (const MediaKind kind : rv::catalog::kMediaKinds) {
            const rv::esde::MediaKindListing& kindListing = systemListing[kind];
            CHECK(kindListing.state != MediaFolderState::Unavailable);
            media[kind] += kindListing.media;
            shadowed[kind] += kindListing.shadowedFiles;
            unlisted += kindListing.unlistedFolders.size();
            unreadable += kindListing.unreadableEntries;
            for (const rv::esde::IgnoredExtension& extension : kindListing.ignored) {
                ignored[folder + "/" + std::string(rv::catalog::toString(kind)) + " " + extension.extension] +=
                    extension.files;
            }
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const rv::esde::MediaListingStats& stats = resolver.stats();

    std::cout << "esde/media real library: " << folders.size() << " system folders, " << stats.files << " files in "
              << stats.folders << " folders, listed in " << seconds << " s (warm: the snapshot just listed the tree; "
              << stats.seconds << " s in the walks), private bytes +" << (privateBytes() - memoryBefore) / 1024 / 1024
              << " MiB\n";
    for (const MediaKind kind : rv::catalog::kMediaKinds) {
        std::cout << "  " << rv::catalog::toString(kind) << ": " << media[kind] << " media, " << shadowed[kind]
                  << " shadowed\n";
    }
    for (const auto& [where, files] : ignored) {
        std::cout << "  ignored: " << where << " " << files << "\n";
    }

    // The survey's census (esde-data.md section 5).
    CHECK(folders.size() == 44);
    CHECK(listedSystems == 44);
    CHECK(stats.systemsListed == 44);
    CHECK(stats.files == 364810);
    CHECK(media[MediaKind::Box3d] == 36250);
    CHECK(media[MediaKind::BackCover] == 31081); // png 31,034, jpg 47
    CHECK(media[MediaKind::Cover] == 36388);     // png 36,287, jpg 101
    CHECK(media[MediaKind::FanArt] == 19636);    // jpg only
    CHECK(media[MediaKind::Manual] == 22768);    // pdf
    CHECK(media[MediaKind::Marquee] == 35705);
    CHECK(media[MediaKind::MixImage] == 39674);
    CHECK(media[MediaKind::PhysicalMedia] == 32569);
    CHECK(media[MediaKind::Screenshot] == 39691);  // png 38,720, jpg 971
    CHECK(media[MediaKind::TitleScreen] == 38235); // png 36,840, jpg 1,395
    CHECK(media[MediaKind::Video] == 32809);       // mp4; the four .flv are not ES-DE's
    CHECK(media[MediaKind::Custom] == 0);          // no custom kind here
    for (const MediaKind kind : rv::catalog::kMediaKinds) {
        CHECK(shadowed[kind] == 0); // no name has both a .png and a .jpg
    }
    CHECK(ignored == std::map<std::string, std::uint64_t>{{"snes/videos .flv", 4}});
    CHECK(unlisted == 0);
    CHECK(unreadable == 0);
}
