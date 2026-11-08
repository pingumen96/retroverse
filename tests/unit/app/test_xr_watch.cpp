// Watching a launched game from VR (I11 step 14, ADR-0020 §6): the menu button held for a second, once per hold, and
// the headset's audio endpoint against Windows' default.

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string_view>

#include <rv/app/xr_watch.hpp>

using rv::app::HoldCommand;
using rv::app::sameAudioOutput;

namespace {

constexpr float kFrame = 1.0f / 60.0f;

/// Frames held until the command fires (0: it never did within `limit`).
int framesToFire(HoldCommand& hold, bool armed, int limit = 200) {
    for (int frame = 1; frame <= limit; ++frame) {
        if (hold.update(armed, true, kFrame)) {
            return frame;
        }
    }
    return 0;
}

} // namespace

TEST_CASE("app/xr_watch: the menu button held for a second fires once per hold", "[app][xr_watch]") {
    SECTION("a second held fires once, at 60 frames") {
        HoldCommand hold;
        CHECK(framesToFire(hold, true) == 60);
        // Still held: nothing more until a release.
        CHECK(framesToFire(hold, true) == 0);
        CHECK_FALSE(hold.update(true, false, kFrame));
        CHECK(framesToFire(hold, true) == 60);
    }
    SECTION("a short press never fires, and the time starts again after a release") {
        HoldCommand hold;
        for (int frame = 0; frame < 50; ++frame) {
            CHECK_FALSE(hold.update(true, true, kFrame));
        }
        CHECK(hold.heldSeconds() > 0.8f);
        CHECK_FALSE(hold.update(true, false, kFrame));
        CHECK(hold.heldSeconds() == 0.0f);
        CHECK(framesToFire(hold, true) == 60);
    }
    SECTION("a hold that began while not armed waits for a release") {
        HoldCommand hold;
        CHECK(framesToFire(hold, false, 30) == 0);
        // Armed while still held (the game started under the held button): nothing until released.
        CHECK(framesToFire(hold, true) == 0);
        CHECK_FALSE(hold.update(true, false, kFrame));
        CHECK(framesToFire(hold, true) == 60);
    }
    SECTION("disarmed during the hold: it forgets it") {
        HoldCommand hold;
        for (int frame = 0; frame < 40; ++frame) {
            CHECK_FALSE(hold.update(true, true, kFrame));
        }
        CHECK_FALSE(hold.update(false, true, kFrame));
        CHECK(hold.heldSeconds() == 0.0f);
        CHECK(framesToFire(hold, true) == 0);
    }
    SECTION("frames of no or bad time add nothing") {
        HoldCommand hold;
        CHECK_FALSE(hold.update(true, true, 0.0f));
        CHECK_FALSE(hold.update(true, true, -1.0f));
        CHECK_FALSE(hold.update(true, true, std::numeric_limits<float>::quiet_NaN()));
        CHECK(hold.heldSeconds() == 0.0f);
        CHECK(hold.update(true, true, 1.0f));
    }
}

TEST_CASE("app/xr_watch: the headset's audio endpoint against Windows' default", "[app][xr_watch]") {
    constexpr std::string_view kHeadset = "{0.0.0.00000000}.{8f2c1e0a-1b2c-4d5e-9f60-718293a4b5c6}";
    constexpr std::string_view kSpeakers = "{0.0.0.00000000}.{11111111-2222-3333-4444-555555555555}";
    CHECK(sameAudioOutput(kHeadset, kHeadset));
    CHECK(sameAudioOutput(kHeadset, "{0.0.0.00000000}.{8F2C1E0A-1B2C-4D5E-9F60-718293A4B5C6}"));
    CHECK_FALSE(sameAudioOutput(kHeadset, kSpeakers));
    // A runtime may give the endpoint's own GUID alone.
    CHECK(sameAudioOutput("{8f2c1e0a-1b2c-4d5e-9f60-718293a4b5c6}", kHeadset));
    CHECK(sameAudioOutput(kHeadset, "{8F2C1E0A-1B2C-4D5E-9F60-718293A4B5C6}"));
    CHECK_FALSE(sameAudioOutput("{11111111-2222-3333-4444-555555555555}", kHeadset));
    // Never a match on nothing, or on a part that is not the GUID.
    CHECK_FALSE(sameAudioOutput("", kHeadset));
    CHECK_FALSE(sameAudioOutput(kHeadset, ""));
    CHECK_FALSE(sameAudioOutput("{0.0.0.00000000}", kHeadset));
    CHECK_FALSE(sameAudioOutput("8f2c1e0a-1b2c-4d5e-9f60-718293a4b5c6}", kHeadset));
}
