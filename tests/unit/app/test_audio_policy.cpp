// The clips' sound as the application decides it (I5 step 5, ADR-0016 §9): the run's output (RV_AUDIO, --audio,
// frame-limited runs), the audible screen, its spatial sound, the duck.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numbers>
#include <optional>
#include <string_view>
#include <vector>

#include <rv/app/audio_policy.hpp>

using Catch::Approx;
using rv::app::AudibleCandidate;
using rv::app::ScreenClass;
using rv::audio::AudioOutput;

TEST_CASE("app/audio_policy: RV_AUDIO forces null or off; then --audio; then null when frame-limited",
          "[app][audio_policy]") {
    struct Row {
        std::optional<AudioOutput> commandLine;
        std::optional<std::string_view> environment;
        bool frameLimited;
        AudioOutput output;
        std::string_view reason;
    };
    const std::vector<Row> table = {
        {std::nullopt, std::nullopt, false, AudioOutput::Device, "interactive"},
        {std::nullopt, std::nullopt, true, AudioOutput::Null, "frame-limited"},
        {AudioOutput::Device, std::nullopt, true, AudioOutput::Device, "--audio"},
        {AudioOutput::Off, std::nullopt, false, AudioOutput::Off, "--audio"},
        // The test presets' RV_AUDIO=null wins over everything: no test can reach the system's audio.
        {AudioOutput::Device, "null", false, AudioOutput::Null, "RV_AUDIO"},
        {std::nullopt, "null", false, AudioOutput::Null, "RV_AUDIO"},
        {AudioOutput::Null, "off", true, AudioOutput::Off, "RV_AUDIO"},
        // RV_AUDIO never raises the output to a device, and other values are ignored.
        {std::nullopt, "device", true, AudioOutput::Null, "frame-limited"},
        {std::nullopt, "DEVICE", true, AudioOutput::Null, "frame-limited"},
        {std::nullopt, "loud", false, AudioOutput::Device, "interactive"},
        {std::nullopt, "", false, AudioOutput::Device, "interactive"},
        {std::nullopt, "   ", false, AudioOutput::Device, "interactive"},
        // Blanks and case do not matter.
        {AudioOutput::Device, " Null ", false, AudioOutput::Null, "RV_AUDIO"},
        {std::nullopt, "OFF\n", false, AudioOutput::Off, "RV_AUDIO"},
        {AudioOutput::Device, "\tnUlL", false, AudioOutput::Null, "RV_AUDIO"},
    };
    for (const Row& row : table) {
        INFO("--audio " << (row.commandLine ? rv::audio::toString(*row.commandLine) : "-") << ", RV_AUDIO "
                        << row.environment.value_or("-") << (row.frameLimited ? ", frame-limited" : ""));
        const rv::app::AudioOutputChoice choice =
            rv::app::resolveAudioOutput(row.commandLine, row.environment, row.frameLimited);
        CHECK(choice.output == row.output);
        CHECK(choice.reason == row.reason);
    }
    // An ignored value is named in a warning (the app logs it once); a used, unset or blank one is not.
    CHECK(rv::app::resolveAudioOutput(std::nullopt, "loud", false).warning ==
          "RV_AUDIO='loud' is ignored: it forces only null or off");
    CHECK(rv::app::resolveAudioOutput(std::nullopt, " Device", false).warning ==
          "RV_AUDIO=device is ignored: it forces only null or off");
    CHECK(rv::app::resolveAudioOutput(std::nullopt, "null", false).warning.empty());
    CHECK(rv::app::resolveAudioOutput(std::nullopt, " ", false).warning.empty());
    CHECK(rv::app::resolveAudioOutput(std::nullopt, std::nullopt, false).warning.empty());
    CHECK(rv::audio::audioOutputFromName("device") == AudioOutput::Device);
    CHECK_FALSE(rv::audio::audioOutputFromName("Device"));
    CHECK(rv::audio::toString(AudioOutput::Off) == "off");
}

TEST_CASE("app/audio_policy: one audible screen: visible and playing, the best class, the nearest, within proximity",
          "[app][audio_policy]") {
    const auto choose = [](const std::vector<AudibleCandidate>& candidates) {
        return rv::app::chooseAudible(candidates, 12.0);
    };
    CHECK_FALSE(choose({}));
    // The hero (room) over a nearer secondary (ambient).
    CHECK(choose({{"crt_a", ScreenClass::Room, 3.8, true, true}, {"crt_c", ScreenClass::Ambient, 2.0, true, true}}) ==
          0u);
    // The focused screen over the hero; direct over focus.
    CHECK(choose({{"crt_a", ScreenClass::Room, 2.0, true, true}, {"crt_b", ScreenClass::Focus, 5.0, true, true},
                  {"crt_c", ScreenClass::Direct, 9.0, true, true}}) == 2u);
    // A class tied: the nearest, then the smallest id.
    CHECK(choose({{"crt_d", ScreenClass::Ambient, 4.0, true, true}, {"crt_c", ScreenClass::Ambient, 3.0, true, true}}) ==
          1u);
    CHECK(choose({{"crt_d", ScreenClass::Ambient, 3.0, true, true}, {"crt_c", ScreenClass::Ambient, 3.0, true, true}}) ==
          1u);
    // Out of view, not playing, or beyond the proximity: silent.
    CHECK(choose({{"crt_a", ScreenClass::Room, 3.0, false, true}, {"crt_c", ScreenClass::Ambient, 4.0, true, true}}) ==
          1u);
    CHECK(choose({{"crt_a", ScreenClass::Room, 3.0, true, false}, {"crt_c", ScreenClass::Ambient, 4.0, true, true}}) ==
          1u);
    CHECK_FALSE(choose({{"crt_a", ScreenClass::Room, 12.0, true, true}}));
    CHECK_FALSE(choose({{"crt_a", ScreenClass::Room, 3.0, false, false}}));
}

TEST_CASE("app/audio_policy: the screen heard keeps the sound unless another is 0.5 m nearer or of a better class",
          "[app][audio_policy]") {
    const auto choose = [](const std::vector<AudibleCandidate>& candidates, std::string_view current) {
        return rv::app::chooseAudible(candidates, 12.0, current);
    };
    // crt_d is heard; crt_c of its class comes nearer: kept until crt_c is 0.5 m nearer.
    CHECK(choose({{"crt_c", ScreenClass::Ambient, 3.9, true, true}, {"crt_d", ScreenClass::Ambient, 4.0, true, true}},
                 "crt_d") == 1u);
    CHECK(choose({{"crt_c", ScreenClass::Ambient, 3.6, true, true}, {"crt_d", ScreenClass::Ambient, 4.0, true, true}},
                 "crt_d") == 1u);
    CHECK(choose({{"crt_c", ScreenClass::Ambient, 3.5, true, true}, {"crt_d", ScreenClass::Ambient, 4.0, true, true}},
                 "crt_d") == 0u);
    // Walking back between them: crt_c stays heard just the same.
    CHECK(choose({{"crt_c", ScreenClass::Ambient, 4.4, true, true}, {"crt_d", ScreenClass::Ambient, 4.0, true, true}},
                 "crt_c") == 0u);
    CHECK(choose({{"crt_c", ScreenClass::Ambient, 4.6, true, true}, {"crt_d", ScreenClass::Ambient, 4.0, true, true}},
                 "crt_c") == 1u);
    // A better class wins at once, however near the screen heard.
    CHECK(choose({{"crt_a", ScreenClass::Room, 9.0, true, true}, {"crt_d", ScreenClass::Ambient, 1.0, true, true}},
                 "crt_d") == 0u);
    // A worse class never takes it, even much nearer.
    CHECK(choose({{"crt_a", ScreenClass::Room, 9.0, true, true}, {"crt_d", ScreenClass::Ambient, 1.0, true, true}},
                 "crt_a") == 0u);
    // The screen heard no longer qualifies (out of view, stopped, beyond the proximity): the best one, at once.
    CHECK(choose({{"crt_c", ScreenClass::Ambient, 3.9, true, true}, {"crt_d", ScreenClass::Ambient, 4.0, false, true}},
                 "crt_d") == 0u);
    CHECK(choose({{"crt_c", ScreenClass::Ambient, 3.9, true, true}, {"crt_d", ScreenClass::Ambient, 4.0, true, false}},
                 "crt_d") == 0u);
    CHECK(choose({{"crt_c", ScreenClass::Ambient, 11.9, true, true}, {"crt_d", ScreenClass::Ambient, 12.0, true, true}},
                 "crt_d") == 0u);
    // None heard so far, or a screen that is gone: the plain choice.
    CHECK(choose({{"crt_c", ScreenClass::Ambient, 3.9, true, true}, {"crt_d", ScreenClass::Ambient, 4.0, true, true}},
                 "") == 0u);
    CHECK(choose({{"crt_c", ScreenClass::Ambient, 3.9, true, true}, {"crt_d", ScreenClass::Ambient, 4.0, true, true}},
                 "crt_x") == 0u);
}

TEST_CASE("app/audio_policy: the azimuth from the eye, the spatial sound, the duck", "[app][audio_policy]") {
    constexpr double kDegree = std::numbers::pi / 180.0;
    // Looking along -Z (yaw 0): a screen ahead, to the right (+X), to the left, behind.
    CHECK(rv::app::azimuthOf(0.0, 0.0, 0.0, -1.0, 0.0, -3.0) == Approx(0.0));
    CHECK(rv::app::azimuthOf(0.0, 0.0, 0.0, -1.0, 3.0, -3.0) == Approx(45.0 * kDegree));
    CHECK(rv::app::azimuthOf(0.0, 0.0, 0.0, -1.0, -3.0, 0.0) == Approx(-90.0 * kDegree));
    CHECK(rv::app::azimuthOf(0.0, 0.0, 0.0, -1.0, 0.0, 3.0) == Approx(180.0 * kDegree));
    // Looking along +X: -Z is on the left.
    CHECK(rv::app::azimuthOf(1.0, 1.0, 1.0, 0.0, 1.0, -2.0) == Approx(-90.0 * kDegree));
    CHECK(rv::app::azimuthOf(0.0, 0.0, 0.0, 0.0, 1.0, 1.0) == 0.0); // no forward direction
    // The four-CRT room's hero seen from the camera: 3.8 m away, 2.1 m to the left.
    const double azimuth = rv::app::azimuthOf(0.0, 0.6, 0.0, -1.0, -2.1, -3.2);
    CHECK(azimuth < 0.0);
    const rv::app::SpatialSound sound = rv::app::spatialSound(3.8, azimuth, 12.0);
    CHECK(sound.gain == Approx(1.5 / 3.8));
    CHECK(sound.pan == Approx(std::sin(azimuth)));
    CHECK(rv::app::spatialSound(13.0, 0.0, 12.0).gain == 0.0);
    CHECK(rv::app::spatialSound(1.0, 90.0 * kDegree, 12.0).pan == Approx(0.6));
    CHECK_FALSE(rv::app::audioDucked(false, false, false));
    CHECK(rv::app::audioDucked(true, false, false));
    CHECK(rv::app::audioDucked(false, true, false));
    CHECK(rv::app::audioDucked(false, false, true));
}
