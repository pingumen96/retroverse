#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string_view>
#include <vector>

#include "support/temp_dir.hpp"
#include <rv/core/file_io.hpp>
#include <rv/core/file_watch_set.hpp>

using namespace std::chrono_literals;
using rv::core::FileWatchSet;

namespace {

/// Manually advanced time source.
struct FakeClock {
    FileWatchSet::Clock::time_point now{std::chrono::hours(1)};

    [[nodiscard]] FileWatchSet::TimeSource source() {
        return [this] {
            return now;
        };
    }
};

void writeFile(const std::filesystem::path& path, std::string_view text) {
    REQUIRE(rv::core::writeTextFileAtomic(path, text).has_value());
}

/// Moves the last write time of `path` forward, so the change does not depend on the file system's timestamp
/// resolution or on how fast the test runs.
void touch(const std::filesystem::path& path, std::chrono::seconds offset = 2s) {
    const auto current = std::filesystem::last_write_time(path);
    std::filesystem::last_write_time(path, current + offset);
}

} // namespace

TEST_CASE("core/file_watch_set: changes are reported once, after the polling interval", "[core][file_watch]") {
    rv::test::TempDir dir;
    const auto shader = dir.file("unlit/unlit.frag");
    writeFile(shader, "v1");
    FakeClock clock;
    FileWatchSet watch(500ms, clock.source());
    watch.add(shader);
    CHECK(watch.size() == 1);
    CHECK(watch.contains(shader));

    touch(shader);
    clock.now += 499ms;
    CHECK(watch.changed().empty()); // throttled: the file system is not even queried

    clock.now += 1ms;
    const std::vector<std::filesystem::path> changed = watch.changed();
    REQUIRE(changed.size() == 1);
    CHECK(changed.front() == shader.lexically_normal());

    clock.now += 500ms;
    CHECK(watch.changed().empty()); // the modification was reported once

    touch(shader);
    clock.now += 200ms;
    CHECK(watch.changed().empty()); // only 200 ms since the previous check
    clock.now += 300ms;
    CHECK(watch.changed().size() == 1);
}

TEST_CASE("core/file_watch_set: unchanged files are not reported", "[core][file_watch]") {
    rv::test::TempDir dir;
    FakeClock clock;
    FileWatchSet watch(500ms, clock.source());
    for (const char* name : {"a.glsl", "b.glsl", "c.glsl"}) {
        writeFile(dir.file(name), name);
        watch.add(dir.file(name));
    }
    for (int i = 0; i < 4; ++i) {
        clock.now += 1s;
        CHECK(watch.changed().empty());
    }
}

TEST_CASE("core/file_watch_set: several changes come back sorted", "[core][file_watch]") {
    rv::test::TempDir dir;
    FakeClock clock;
    FileWatchSet watch(500ms, clock.source());
    for (const char* name : {"c.glsl", "a.glsl", "b.glsl"}) {
        writeFile(dir.file(name), name);
        watch.add(dir.file(name));
    }
    touch(dir.file("c.glsl"));
    touch(dir.file("a.glsl"));
    clock.now += 1s;
    const auto changed = watch.changed();
    REQUIRE(changed.size() == 2);
    CHECK(changed[0] == dir.file("a.glsl").lexically_normal());
    CHECK(changed[1] == dir.file("c.glsl").lexically_normal());
}

TEST_CASE("core/file_watch_set: files that appear or disappear are changes", "[core][file_watch]") {
    rv::test::TempDir dir;
    const auto file = dir.file("common/new.glsl");
    FakeClock clock;
    FileWatchSet watch(500ms, clock.source());
    watch.add(file); // missing for now
    clock.now += 1s;
    CHECK(watch.changed().empty());

    writeFile(file, "created");
    clock.now += 1s;
    CHECK(watch.changed().size() == 1);

    std::filesystem::remove(file);
    clock.now += 1s;
    CHECK(watch.changed().size() == 1);
    clock.now += 1s;
    CHECK(watch.changed().empty());
}

TEST_CASE("core/file_watch_set: adding a watched file again keeps its reference", "[core][file_watch]") {
    rv::test::TempDir dir;
    const auto file = dir.file("a.glsl");
    writeFile(file, "a");
    FakeClock clock;
    FileWatchSet watch(500ms, clock.source());
    watch.add(file);
    touch(file);
    watch.add(file);                                 // must not swallow the pending modification
    watch.add(dir.path() / "sub" / ".." / "a.glsl"); // same file, different spelling
    CHECK(watch.size() == 1);
    clock.now += 1s;
    CHECK(watch.changed().size() == 1);
}

TEST_CASE("core/file_watch_set: assign keeps references and drops unlisted files", "[core][file_watch]") {
    rv::test::TempDir dir;
    for (const char* name : {"a.glsl", "b.glsl", "c.glsl"}) {
        writeFile(dir.file(name), name);
    }
    FakeClock clock;
    FileWatchSet watch(500ms, clock.source());
    watch.add(dir.file("a.glsl"));
    watch.add(dir.file("b.glsl"));
    touch(dir.file("a.glsl"));
    touch(dir.file("b.glsl"));

    const std::vector<std::filesystem::path> files{dir.file("a.glsl"), dir.file("c.glsl"), dir.file("a.glsl")};
    watch.assign(files);
    CHECK(watch.size() == 2);
    CHECK(watch.contains(dir.file("a.glsl")));
    CHECK_FALSE(watch.contains(dir.file("b.glsl")));
    CHECK(watch.contains(dir.file("c.glsl")));

    clock.now += 1s;
    const auto changed = watch.changed(); // a's pending change survives; c starts from its current state
    REQUIRE(changed.size() == 1);
    CHECK(changed.front() == dir.file("a.glsl").lexically_normal());
}

TEST_CASE("core/file_watch_set: remove, clear and interval edge cases", "[core][file_watch]") {
    rv::test::TempDir dir;
    const auto file = dir.file("a.glsl");
    writeFile(file, "a");

    SECTION("removed files are no longer checked") {
        FakeClock clock;
        FileWatchSet watch(500ms, clock.source());
        watch.add(file);
        watch.remove(dir.file("missing.glsl")); // unknown: no-op
        CHECK(watch.size() == 1);
        watch.remove(file);
        CHECK(watch.empty());
        touch(file);
        clock.now += 1s;
        CHECK(watch.changed().empty());

        watch.add(file);
        watch.clear();
        CHECK(watch.empty());
    }
    SECTION("a zero or negative interval checks on every call") {
        FakeClock clock;
        FileWatchSet watch(-5ms, clock.source());
        watch.add(file);
        touch(file);
        CHECK(watch.changed().size() == 1);
        touch(file);
        CHECK(watch.changed().size() == 1);
    }
    SECTION("a clock going backwards restarts the interval") {
        FakeClock clock;
        FileWatchSet watch(500ms, clock.source());
        watch.add(file);
        touch(file);
        clock.now -= 10s;
        CHECK(watch.changed().empty());
        clock.now += 499ms;
        CHECK(watch.changed().empty());
        clock.now += 1ms;
        CHECK(watch.changed().size() == 1);
    }
    SECTION("the default time source is the steady clock") {
        FileWatchSet watch(0ms);
        watch.add(file);
        touch(file);
        CHECK(watch.changed().size() == 1);
    }
}
