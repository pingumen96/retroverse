#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <glm/geometric.hpp>

#include <rv/core/math.hpp>
#include <rv/xr/fake_runtime.hpp>
#include <rv/xr/session.hpp>

using Catch::Approx;
using rv::xr::FakeRuntime;
using rv::xr::FakeRuntimeConfig;
using rv::xr::Session;
using rv::xr::SessionState;

namespace {

struct Fixture {
    FakeRuntime* runtime = nullptr;
    std::unique_ptr<Session> session;
};

Fixture makeSession(FakeRuntimeConfig config = {}) {
    auto runtime = std::make_unique<FakeRuntime>(std::move(config));
    Fixture fixture;
    fixture.runtime = runtime.get();
    fixture.session = std::make_unique<Session>(std::move(runtime));
    return fixture;
}

/// Runs `count` frames, each rendered when the session wants its views.
void runFrames(Session& session, int count) {
    for (int i = 0; i < count; ++i) {
        session.pollEvents();
        auto frame = session.beginFrame();
        REQUIRE(frame.has_value());
        REQUIRE(session.endFrame(frame->has_value() && (*frame)->wantsViews()).has_value());
    }
}

std::vector<std::string> eventNames(const Session& session) {
    std::vector<std::string> names;
    for (const rv::xr::SessionEvent& event : session.events()) {
        names.push_back(event.what);
    }
    return names;
}

} // namespace

TEST_CASE("xr/session: the fake runtime's session begins when ready and is focused (I10 step 2)", "[xr][session]") {
    Fixture f = makeSession();
    CHECK_FALSE(f.session->running());
    // Not running yet: no frame.
    auto before = f.session->beginFrame();
    REQUIRE(before.has_value());
    CHECK_FALSE(before->has_value());

    f.session->pollEvents();
    CHECK(f.session->running());
    CHECK(f.session->state() == SessionState::Focused);
    CHECK(f.session->reachedFocused());
    CHECK(eventNames(*f.session) == std::vector<std::string>{"idle", "ready", "synchronized", "visible", "focused"});

    auto frame = f.session->beginFrame();
    REQUIRE(frame.has_value());
    REQUIRE(frame->has_value());
    const rv::xr::Frame& first = **frame;
    CHECK(first.timing.shouldRender);
    CHECK(first.located);
    CHECK(first.wantsViews());
    CHECK(first.images[0] != first.images[1]);
    CHECK(first.eyeSize == glm::ivec2(1024, 1024));
    REQUIRE(f.session->endFrame(true).has_value());
    CHECK(f.runtime->framesWithViews() == 1);
}

TEST_CASE("xr/session: frames at the runtime's pace, each eye in its own place and field (I10 step 2)",
          "[xr][session]") {
    FakeRuntimeConfig config;
    config.renderScale = 0.5;
    config.headPose = [](std::uint64_t frame) -> std::optional<rv::xr::Pose> {
        // The head turns 1 degree left per frame, 1.7 m up.
        const float yaw = rv::core::degreesToRadians(-static_cast<float>(frame));
        return rv::xr::Pose{rv::xr::orientationOf(rv::xr::Angles{yaw, 0.0f, 0.0f}), glm::vec3(0.0f, 1.7f, 0.0f)};
    };
    Fixture f = makeSession(config);
    f.session->pollEvents();
    for (int i = 0; i < 3; ++i) {
        auto frame = f.session->beginFrame();
        REQUIRE(frame.has_value());
        REQUIRE(frame->has_value());
        const rv::xr::EyeViews& eyes = (*frame)->eyes;
        CHECK((*frame)->eyeSize == glm::ivec2(512, 512));
        CHECK(glm::distance(eyes[0].pose.position, eyes[1].pose.position) == Approx(0.064f));
        const float yaw = rv::core::degreesToRadians(-static_cast<float>(i));
        CHECK(rv::xr::anglesOf(eyes[0].pose.orientation).yaw == Approx(yaw));
        // The left eye sees farther to the left, and the right eye to the right.
        CHECK(eyes[0].fov.left < -eyes[0].fov.right);
        CHECK(eyes[1].fov.right == Approx(-eyes[0].fov.left));
        REQUIRE(f.session->endFrame(true).has_value());
    }
    const rv::xr::FrameSummary summary = f.session->frameSummary();
    CHECK(summary.frames == 3);
    CHECK(summary.rendered == 3);
    CHECK(summary.refreshHz == Approx(72.0).margin(0.01));
    CHECK(summary.missedRefreshes == 0);
    // The swapchains' images go round.
    CHECK(f.session->framesWaited() == 3);
}

TEST_CASE("xr/session: lost tracking submits no layer, and the frame is still counted (I10 step 2)",
          "[xr][session]") {
    FakeRuntimeConfig config;
    config.untrackedFrames = {1};
    Fixture f = makeSession(config);
    f.session->pollEvents();
    std::vector<bool> wanted;
    for (int i = 0; i < 3; ++i) {
        auto frame = f.session->beginFrame();
        REQUIRE(frame.has_value());
        REQUIRE(frame->has_value());
        wanted.push_back((*frame)->wantsViews());
        CHECK((*frame)->located == ((*frame)->wantsViews()));
        REQUIRE(f.session->endFrame((*frame)->wantsViews()).has_value());
    }
    CHECK(wanted == std::vector<bool>{true, false, true});
    CHECK(f.runtime->framesEnded() == 3);
    CHECK(f.runtime->framesWithViews() == 2);
    CHECK(f.session->frameSummary().rendered == 2);
}

TEST_CASE("xr/session: an exit request stops, ends and exits the session (I10 step 2)", "[xr][session]") {
    Fixture f = makeSession();
    runFrames(*f.session, 2);
    f.session->requestExit();
    f.session->pollEvents();
    CHECK_FALSE(f.session->running());
    CHECK(f.session->ended());
    CHECK(f.session->endReason() == "exited");
    const std::vector<std::string> names = eventNames(*f.session);
    REQUIRE(names.size() >= 4);
    CHECK(std::vector<std::string>(names.end() - 4, names.end()) ==
          std::vector<std::string>{"exit-requested", "stopping", "idle", "exiting"});
    auto after = f.session->beginFrame();
    REQUIRE(after.has_value());
    CHECK_FALSE(after->has_value());
}

TEST_CASE("xr/session: a lost session ends, and frames stop (I10 step 2)", "[xr][session]") {
    FakeRuntimeConfig config;
    config.loseAfterFrames = 5;
    Fixture f = makeSession(config);
    runFrames(*f.session, 6);
    CHECK(f.session->ended());
    CHECK(f.session->endReason() == "lost");
    // The 6th frame found the session over: 5 waited for.
    CHECK(f.session->framesWaited() == 5);
    CHECK(f.session->events().back().what == "loss-pending");
    CHECK(f.session->events().back().frame == 5);
}

TEST_CASE("xr/session: a failed call ends the session with its reason (I10 step 2)", "[xr][session]") {
    Fixture f = makeSession();
    f.session->pollEvents();
    auto frame = f.session->beginFrame();
    REQUIRE(frame.has_value());
    // A second begin without an end: the fake runtime refuses to acquire images twice.
    auto again = f.session->beginFrame();
    REQUIRE_FALSE(again.has_value());
    CHECK(f.session->ended());
    CHECK(f.session->endReason().find("acquireImage") != std::string::npos);
}

TEST_CASE("xr/session: a recentering reaches the next located frame once (I10 step 3)", "[xr][session]") {
    FakeRuntimeConfig config;
    config.recenterFrames = {2};
    config.untrackedFrames = {3};
    Fixture f = makeSession(std::move(config));
    std::vector<bool> changed;
    std::vector<bool> located;
    for (int i = 0; i < 6; ++i) {
        f.session->pollEvents();
        auto frame = f.session->beginFrame();
        REQUIRE(frame.has_value());
        REQUIRE(frame->has_value());
        changed.push_back((*frame)->spaceChanged);
        located.push_back((*frame)->located);
        REQUIRE(f.session->endFrame((*frame)->wantsViews()).has_value());
    }
    // Waited for at frame 2, polled before frame 3, which is not located: frame 4 carries it.
    CHECK(located == std::vector<bool>{true, true, true, false, true, true});
    CHECK(changed == std::vector<bool>{false, false, false, false, true, false});
    const std::vector<std::string> names = eventNames(*f.session);
    CHECK(std::count(names.begin(), names.end(), "reference-space-change") == 1);
}
