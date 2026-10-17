// The quick-hash pass on a sample of the real library (I2 step 11b, ADR-0008 §10). Hidden ([.real-library]): ctest
// never runs it; scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the library before and after,
// and the folders in environment variables (RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL, RV_REAL_ROM_ROOT,
// RV_REAL_MEDIA_ROOT; see test_import_real.cpp). One system (gb) is imported into a new database under the build tree
// (build/<preset>/real-library/reconciliation), then hashed: every present file read once, at most 128 KiB each, never
// written. A second pass hashes nothing; an import after it pairs nothing (the library does not change). The times are
// informative (the drive may be warm from the script's snapshots); the whole library's pass is estimated from the
// sample's rates and the whole snapshot's files.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include <windows.h>

#include <fmt/format.h>

#include <rv/catalog/fingerprint.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>

#include "import_test_support.hpp"
#include "support/fixture.hpp"

namespace fs = std::filesystem;
using namespace rv::catalog;

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

double seconds(std::chrono::nanoseconds time) {
    return std::chrono::duration<double>(time).count();
}

constexpr double kMiB = 1024.0 * 1024.0;

} // namespace

TEST_CASE("catalog_db/reconciliation: the quick-hash pass on the real library's gb, then an import that pairs nothing",
          "[.real-library][real-library][reconciliation]") {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    const auto romRoot = pathVariable(L"RV_REAL_ROM_ROOT");
    const auto mediaRoot = pathVariable(L"RV_REAL_MEDIA_ROOT");
    if (!home || !install || !romRoot || !mediaRoot) {
        SKIP("set RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL, RV_REAL_ROM_ROOT and RV_REAL_MEDIA_ROOT "
             "(scripts/real-library.ps1 does)");
    }
    LibraryProfile profile;
    profile.esdeHome = *home;
    profile.installFolder = *install;
    profile.romRoot = *romRoot;
    profile.mediaRoot = *mediaRoot;
    const SystemName gb = rv::test::systemName("gb");

    // A new database under the build tree, never in the library.
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "reconciliation";
    std::error_code removed;
    fs::remove_all(folder, removed);
    REQUIRE_FALSE(removed);
    rv::test::ImportCatalog catalog = rv::test::ImportCatalog::open(folder / "retroverse.sqlite");
    const auto settings = rv::esde::readSettings(profile.esdeHome);
    REQUIRE(settings.has_value());
    const auto importGb = [&](IdentityPairing& pairing) {
        rv::esde::EsdeLibrarySourceOptions options;
        options.systems = {"gb"};
        rv::esde::EsdeLibrarySource source(std::move(options));
        rv::esde::EsdeMediaResolver media(rv::esde::resolveMediaRoot(*settings, profile));
        ImportOptions importOptions;
        importOptions.systems = {gb};
        return rv::test::importOk(catalog, source, media, profile, importOptions, &pairing);
    };
    IdentityPairing firstPairing;
    const ImportResult first = importGb(firstPairing);
    const SystemChanges& changes = rv::test::changesOf(first.changes, "gb");
    CHECK(changes.newIdentities > 8'000);
    CHECK(firstPairing.statistics().requests == 0); // a first import reads no ROM byte

    // The pass: every present file of gb, read once.
    QuickHashPassOptions options;
    options.system = gb;
    const QuickHashPassResult pass = runQuickHashPass(*catalog.store, profile.id, options).value();
    REQUIRE(pass.batches > 0);
    CHECK(pass.hashed > 8'000);
    CHECK(pass.remaining == 0);
    CHECK(pass.hashed + pass.changed + pass.gone + pass.unreadable + pass.unavailable == pass.candidates);
    CHECK(pass.unavailable == 0);
    CHECK(pass.bytesRead <= static_cast<std::uint64_t>(pass.hashed) * kQuickHashWholeFileLimit);
    const double passSeconds = seconds(pass.time);
    std::cout << fmt::format(
        "quick-hash pass, gb: {} identities ({} not present, {} folders), {} files hashed, {} "
        "changed, {} gone, {} unreadable; {:.1f} MiB read in {:.2f} s ({} batches): {:.0f} files/s, "
        "{:.1f} MiB/s\n",
        pass.identities, pass.notPresent, pass.folders, pass.hashed, pass.changed, pass.gone, pass.unreadable,
        static_cast<double>(pass.bytesRead) / kMiB, passSeconds, pass.batches,
        static_cast<double>(pass.hashed) / passSeconds, static_cast<double>(pass.bytesRead) / kMiB / passSeconds);
    for (const Issue& issue : pass.issues) {
        std::cout << fmt::format("  [{}] {}: {}\n", toString(issue.severity), issue.code, issue.reason);
    }

    // A second pass: nothing left to hash.
    const QuickHashPassResult again = runQuickHashPass(*catalog.store, profile.id, options).value();
    CHECK(again.hashed == 0);
    CHECK(again.candidates == pass.candidates - pass.hashed);
    CHECK(again.current == pass.current + pass.hashed);
    std::cout << fmt::format("second pass, gb: {} files hashed in {:.2f} s\n", again.hashed, seconds(again.time));

    // The library did not change: an import pairs nothing, writes nothing, reads no ROM byte.
    IdentityPairing pairing;
    const ImportResult steady = importGb(pairing);
    CHECK(steady.changes.moved.empty());
    CHECK(steady.changes.reviews.empty());
    CHECK(steady.statistics.identitiesWritten == 0);
    CHECK(steady.statistics.entriesWritten == 0);
    CHECK(steady.statistics.historyWritten == 0);
    CHECK(pairing.statistics().filesHashed == 0);

    // The whole library's pass, estimated from the sample: its present files (games, members, files that are no
    // games, never folders) and the bytes their quick hashes read.
    rv::esde::EsdeLibrarySource whole;
    const auto snapshot = whole.load(profile, LibraryLoadOptions{});
    REQUIRE(snapshot.has_value());
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
    for (const CatalogEntry& entry : snapshot->entries) {
        if (entry.key.empty() || !entry.file || entry.file->directory ||
            entry.availability != Availability::Available) {
            continue;
        }
        ++files;
        bytes += quickHashBytes(entry.file->size);
    }
    const double perFile = passSeconds / static_cast<double>(pass.hashed);
    const double perByte = passSeconds / static_cast<double>(pass.bytesRead);
    std::cout << fmt::format(
        "whole library: {} files, {:.1f} MiB to read for their quick hashes; estimated {:.0f} s at "
        "gb's rate per file, {:.0f} s at its rate per byte\n",
        files, static_cast<double>(bytes) / kMiB, perFile * static_cast<double>(files),
        perByte * static_cast<double>(bytes));
    CHECK(files > 100'000);
}
