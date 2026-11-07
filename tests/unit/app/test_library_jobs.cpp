// The catalog worker's jobs for the coverage and library screens (I7 step 8, ADR-0018 §14) on the standard ES-DE fixture:
// the coverage of the catalog's rows with the clips known to fail; the library's folders, last import and quick hashes;
// a review row resolved only to one of its own candidates; the removals the guard holds applied only by a refresh that
// accepts them (the screen's confirmation); the quick-hash pass stopped in the middle and resumed from what is left.
// Everything is written under the build tree; nothing launches.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rv/app/catalog_worker.hpp>
#include <rv/app/library_data.hpp>
#include <rv/catalog/fingerprint.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/core/utf8.hpp>
#include <rv/media/video_verdicts.hpp>

#include "catalog_test_support.hpp"

namespace fs = std::filesystem;
using rv::app::CatalogResults;
using rv::app::CatalogState;
using rv::app::CatalogWorker;
using rv::test::ImportedFixture;

namespace {

const rv::catalog::LibraryId kLibrary = rv::catalog::LibraryId::parse("esde").value();

/// A worker on the fixture, its opening waited for, what it answers gathered.
struct Worker {
    explicit Worker(const ImportedFixture& fixture, rv::catalog::QuickHashReader reader = {})
        : worker(optionsOf(fixture, std::move(reader))) {
        wait([&] { return all.opened.has_value(); });
        REQUIRE(all.opened->state == CatalogState::Ready);
    }

    static CatalogWorker::Options optionsOf(const ImportedFixture& fixture, rv::catalog::QuickHashReader reader) {
        CatalogWorker::Options options;
        options.paths = fixture.paths;
        options.owner = rv::catalog::SessionOwner{ImportedFixture::testOwnerProcessId(), 1};
        options.hashReader = std::move(reader);
        return options;
    }

    void gather() {
        CatalogResults results = worker.take();
        if (results.opened) {
            all.opened = std::move(results.opened);
        }
        if (results.coverage) {
            all.coverage = std::move(results.coverage);
        }
        if (results.libraryStatus) {
            all.libraryStatus = std::move(results.libraryStatus);
        }
        if (results.refreshed) {
            all.refreshed = std::move(results.refreshed);
        }
        if (results.reviewed) {
            all.reviewed = std::move(results.reviewed);
        }
        if (results.hashProgress) {
            all.hashProgress = results.hashProgress;
        }
        if (results.hashEnded) {
            all.hashEnded = std::move(results.hashEnded);
        }
        for (auto& resolved : results.reviewsResolved) {
            all.reviewsResolved.push_back(std::move(resolved));
        }
    }

    template <typename Done>
    void wait(Done done) {
        const auto deadline = std::chrono::steady_clock::now() + rv::test::kSettleTimeout;
        while (true) {
            gather();
            if (done()) {
                return;
            }
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    rv::app::LibraryStatus status() {
        all.libraryStatus.reset();
        worker.libraryStatus(++serial);
        wait([&] { return all.libraryStatus.has_value(); });
        REQUIRE_FALSE(all.libraryStatus->error.has_value());
        return *all.libraryStatus;
    }

    rv::app::RefreshSummary refresh(rv::app::RefreshRequest request) {
        all.refreshed.reset();
        REQUIRE(worker.refresh(std::move(request)));
        wait([&] { return all.refreshed.has_value(); });
        return all.refreshed->summary;
    }

    rv::app::ReviewResolved resolve(std::int64_t review, std::optional<rv::catalog::GameId> with) {
        all.reviewsResolved.clear();
        rv::catalog::ReviewDecision decision;
        decision.review = review;
        decision.pairWith = with;
        decision.force = true; // what a caller asks: the worker never forces
        worker.resolveReview(++serial, decision);
        wait([&] { return !all.reviewsResolved.empty(); });
        return all.reviewsResolved.front();
    }

    rv::app::QuickHashEnded hashEnd() {
        wait([&] { return all.hashEnded.has_value(); });
        rv::app::QuickHashEnded ended = std::move(*all.hashEnded);
        all.hashEnded.reset();
        return ended;
    }

    CatalogWorker worker;
    CatalogResults all;
    std::uint64_t serial = 0;
};

fs::path romFolder(const ImportedFixture& fixture, std::string_view system) {
    return fixture.fixture.path(std::string("roms/") + std::string(system));
}

/// `from` copied to `to`, its last write time kept (a move that keeps both: what size, time and name pair).
void copyKeepingTime(const fs::path& from, const fs::path& to) {
    fs::create_directories(to.parent_path());
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
    fs::last_write_time(to, fs::last_write_time(from));
}

} // namespace

TEST_CASE("app/library: the coverage of the catalog's rows with the clips known to fail", "[app][library][coverage]") {
    const ImportedFixture fixture("rv-library-coverage");
    // Two failure verdicts and a playable one, as the screens and media --check-videos write them.
    std::vector<rv::media::VideoVerdict> verdicts;
    rv::media::VideoVerdict bad;
    bad.path = "gb/videos/Tiny Tennis (Europe).mp4";
    bad.file = rv::media::VideoFileFacts{1000, 133'700'000'000'000'000};
    bad.failure = rv::media::DecodeFailure::Truncated;
    verdicts.push_back(bad);
    bad.path = "snes/videos/Alpha Quest (USA).mp4";
    bad.failure = rv::media::DecodeFailure::CorruptData;
    verdicts.push_back(bad);
    rv::media::VideoVerdict good = bad;
    good.path = "snes/videos/Delta Drift (USA).mp4";
    good.failure = rv::media::DecodeFailure::None;
    verdicts.push_back(good);
    REQUIRE(rv::media::updateVideoVerdicts(rv::media::videoVerdictsFile(fixture.paths.cacheDir()), verdicts)
                .has_value());

    Worker worker(fixture);
    worker.worker.coverage(1);
    worker.wait([&] { return worker.all.coverage.has_value(); });
    const rv::app::CoverageList& list = *worker.all.coverage;
    REQUIRE_FALSE(list.error.has_value());
    CHECK_FALSE(list.clipsError.has_value());
    CHECK(list.milliseconds < 1000.0);

    auto database = fixture.openDatabase();
    auto reader = database.openReader();
    REQUIRE(reader.has_value());
    auto query = rv::catalog_db::SqliteCatalogQuery::open(std::move(*reader));
    REQUIRE(query.has_value());
    const auto direct = (*query)->mediaCoverage(kLibrary);
    REQUIRE(direct.has_value());
    CHECK(list.coverage == *direct);

    REQUIRE(list.clips.size() == 2);
    CHECK(list.clips[0] == rv::app::ClipFailure{"gb", "Tiny Tennis (Europe).mp4", "truncated"});
    CHECK(list.clips[1] == rv::app::ClipFailure{"snes", "Alpha Quest (USA).mp4", "corrupt-data"});
    const auto gb = std::find_if(list.fullNames.begin(), list.fullNames.end(),
                                 [](const auto& entry) { return entry.first == "gb"; });
    REQUIRE(gb != list.fullNames.end());
    CHECK(gb->second == "Nintendo Game Boy");

    // As the screen shows it: worst first, the clips counted per system.
    const rv::ui::CoverageContent content = rv::app::coverageContentOf(list);
    CHECK_FALSE(content.empty);
    CHECK(content.games.total.games == list.coverage.total[rv::catalog::CoveragePopulation::Games].entries);
    REQUIRE(content.games.systems.size() == list.coverage.systems.size());
    const auto gbRow = std::find_if(content.games.systems.begin(), content.games.systems.end(),
                                    [](const rv::ui::CoverageRow& row) { return row.name == "gb"; });
    REQUIRE(gbRow != content.games.systems.end());
    CHECK(gbRow->fullName == "Nintendo Game Boy");
    CHECK(gbRow->clipFailures == 1);
    CHECK(gbRow->kinds.size() == rv::catalog::kMediaKindCount);
    CHECK(content.clips.size() == 2);
}

TEST_CASE("app/library: the folders, the last import and the quick hashes", "[app][library]") {
    const ImportedFixture fixture("rv-library-status");
    Worker worker(fixture);
    const rv::app::LibraryStatus status = worker.status();
    CHECK(status.milliseconds < 1000.0);
    REQUIRE(status.folders.size() >= 3);
    CHECK(status.folders.front().kind == "esde-home");
    for (const rv::app::LibraryFolder& folder : status.folders) {
        INFO(folder.kind);
        CHECK(folder.reachable == std::optional<bool>(true));
    }
    CHECK(std::any_of(status.folders.begin(), status.folders.end(),
                      [](const rv::app::LibraryFolder& folder) { return folder.kind == "rom"; }));
    CHECK(std::any_of(status.folders.begin(), status.folders.end(),
                      [](const rv::app::LibraryFolder& folder) { return folder.kind == "media"; }));
    REQUIRE(status.lastImport.has_value());
    CHECK(status.lastImport->kind == rv::catalog::ImportKind::Import);
    CHECK(status.held.empty());
    CHECK(status.reviews.empty());
    CHECK(status.hashes.files > 0);
    CHECK(status.hashes.current == 0);
    const rv::ui::LibraryContent content =
        rv::app::libraryContentOf(status, [](rv::catalog::UtcTime) { return std::optional<rv::ui::DirectoryDate>(); });
    CHECK(content.lastImport.known);
    CHECK(content.files == status.hashes.files);
    CHECK(content.issueCount == status.issues.size());
}

TEST_CASE("app/library: a review row is resolved only to one of its own candidates", "[app][library]") {
    const ImportedFixture fixture("rv-library-review");
    const fs::path gb = romFolder(fixture, "gb");
    const auto vanished = fixture.gameAt("gb", "Tiny Tennis (Europe).gb");
    const auto other = fixture.gameAt("gb", "Café Racer (Europe).gb");
    // Moved into two folders at once, size, time and name kept: no rule can tell which it is.
    copyKeepingTime(gb / "Tiny Tennis (Europe).gb", gb / "A" / "Tiny Tennis (Europe).gb");
    copyKeepingTime(gb / "Tiny Tennis (Europe).gb", gb / "B" / "Tiny Tennis (Europe).gb");
    fs::remove(gb / "Tiny Tennis (Europe).gb");

    Worker worker(fixture);
    const rv::app::RefreshSummary refreshed = worker.refresh(rv::app::RefreshRequest{});
    REQUIRE(refreshed.outcome == rv::app::RefreshOutcome::Done);
    CHECK(refreshed.reviews == 1);
    rv::app::LibraryStatus status = worker.status();
    REQUIRE(status.reviews.size() == 1);
    const rv::app::ReviewState review = status.reviews.front();
    CHECK(review.game == vanished);
    CHECK(review.vanished == "Tiny Tennis (Europe).gb");
    CHECK(review.title == "Tiny Tennis");
    REQUIRE(review.candidates.size() == 2);
    CHECK(review.candidates[0].path == "A/Tiny Tennis (Europe).gb");
    CHECK(review.candidates[1].path == "B/Tiny Tennis (Europe).gb");
    REQUIRE(review.candidates[0].game.has_value());
    REQUIRE(review.candidates[1].game.has_value());

    // Another game of the system, even asked with force: refused, nothing changed.
    const rv::app::ReviewResolved refused = worker.resolve(review.id, other);
    CHECK(refused.problem == "refused");
    CHECK_FALSE(refused.paired);
    CHECK(worker.status().reviews.size() == 1);
    // A review that does not exist.
    CHECK(worker.resolve(review.id + 100, review.candidates[0].game).problem == "conflict");

    // Its candidate A: paired, the systems resolved again for the lists and the building.
    worker.all.reviewed.reset();
    const rv::app::ReviewResolved paired = worker.resolve(review.id, review.candidates[0].game);
    CHECK(paired.problem.empty());
    CHECK(paired.paired);
    worker.wait([&] { return worker.all.reviewed.has_value(); });
    CHECK(worker.status().reviews.empty());
    CHECK(fixture.gameAt("gb", "A/Tiny Tennis (Europe).gb") == vanished);
    // Resolved already.
    CHECK(worker.resolve(review.id, review.candidates[1].game).problem == "conflict");
}

TEST_CASE("app/library: the removals the guard holds are applied only by a refresh that accepts them",
          "[app][library]") {
    const ImportedFixture fixture("rv-library-removals");
    const fs::path snes = romFolder(fixture, "snes");
    for (const char* name : {"Alpha Quest (USA).sfc", "Beta Run (Europe).SMC", "Delta Drift (USA).sfc",
                             "Hacks/Gamma Hack (Hack).sfc", "Sports Vol. 2/Ice Lanes (USA).sfc"}) {
        REQUIRE(fs::remove(snes / fs::path(rv::core::pathFromUtf8(name))));
    }
    Worker worker(fixture);
    const auto before = worker.status();
    CHECK(before.held.empty());
    // A refresh as the watcher or Aggiorna libreria asks it: held, nothing removed.
    const rv::app::RefreshSummary held = worker.refresh(rv::app::RefreshRequest{});
    REQUIRE(held.outcome == rv::app::RefreshOutcome::Done);
    CHECK(held.guarded == 1);
    CHECK(held.removed == 0);
    rv::app::LibraryStatus status = worker.status();
    REQUIRE(status.held.size() == 1);
    CHECK(status.held.front().system.str() == "snes");
    CHECK(status.held.front().gone == 5);
    CHECK(status.held.front().present >= 5);
    const rv::ui::LibraryContent content = rv::app::libraryContentOf(status, {});
    REQUIRE(content.held.size() == 1);
    CHECK(content.held.front().gone == 5);
    CHECK(std::any_of(content.issues.begin(), content.issues.end(),
                      [](const rv::ui::LibraryIssueGroup& group) { return group.code == "guard"; }));
    // Again without accepting: still held.
    CHECK(worker.refresh(rv::app::RefreshRequest{}).guarded == 1);
    CHECK(worker.status().held.size() == 1);
    // "Applica le rimozioni", confirmed: the held systems' refresh that accepts them.
    rv::app::RefreshRequest apply;
    apply.systems = {status.held.front().system};
    apply.acceptRemovals = true;
    const rv::app::RefreshSummary applied = worker.refresh(apply);
    REQUIRE(applied.outcome == rv::app::RefreshOutcome::Done);
    CHECK(applied.guarded == 0);
    CHECK(applied.removed > 0);
    CHECK(worker.status().held.empty());
}

TEST_CASE("app/library: the quick-hash pass stopped in the middle goes on with what is left", "[app][library]") {
    const ImportedFixture fixture("rv-library-hash");
    // The third file waits until the test has asked the pass to stop.
    std::mutex mutex;
    std::condition_variable released;
    bool release = false;
    std::atomic<int> reads{0};
    const auto reader = [&](const fs::path& folder, std::string_view relative) {
        if (++reads == 3) {
            std::unique_lock lock(mutex);
            released.wait_for(lock, std::chrono::seconds(20), [&] { return release; });
        }
        return rv::catalog::readQuickHashBelow(folder, relative);
    };
    Worker worker(fixture, reader);
    const std::size_t files = worker.status().hashes.files;
    REQUIRE(files > 3);

    REQUIRE(worker.worker.quickHash());
    CHECK_FALSE(worker.worker.quickHash()); // one at a time
    worker.wait([&] { return reads.load() >= 3; });
    // A list and the library's state are answered meanwhile (between two files).
    worker.worker.cancelQuickHash();
    {
        const std::lock_guard lock(mutex);
        release = true;
    }
    released.notify_all();
    const rv::app::QuickHashEnded stopped = worker.hashEnd();
    CHECK(stopped.outcome == "cancelled");
    CHECK(stopped.result.cancelled);
    CHECK(stopped.result.hashed == 3);
    CHECK(stopped.result.remaining == files - 3);
    CHECK(worker.status().hashes.current == 3);

    // Resumed: what is left, nothing read twice.
    REQUIRE(worker.worker.quickHash());
    const rv::app::QuickHashEnded done = worker.hashEnd();
    CHECK(done.outcome == "done");
    CHECK_FALSE(done.result.cancelled);
    CHECK(done.result.current == 3);
    CHECK(done.result.hashed == files - 3);
    CHECK(done.result.remaining == 0);
    CHECK(reads.load() == static_cast<int>(files));
    const rv::app::LibraryStatus status = worker.status();
    CHECK(status.hashes.current == files);
    CHECK(status.hashes.missing() == 0);
}
