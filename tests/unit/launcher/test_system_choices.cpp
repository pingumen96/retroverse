// I2 step 10: emulators.json's "systemChoices", Retroverse's choice for a system (ADR-0008 §6, §8): a system mapped to
// an ES-DE command label or a profile id, each choice validated on its own, the user's document replacing the shipped
// one's per system.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <nlohmann/json.hpp>

#include <rv/launcher/emulator_profile.hpp>

using rv::launcher::LaunchChoice;
using rv::launcher::LaunchChoiceKind;
using rv::launcher::ProfileOrigin;

namespace {

nlohmann::json minimalProfile() {
    return nlohmann::json{{"id", "emu"}, {"executable", "C:\\Emu\\emu.exe"}, {"arguments", {"{rom}"}}};
}

rv::launcher::ProfileDocument parse(const nlohmann::json& choices, ProfileOrigin origin = ProfileOrigin::User) {
    const nlohmann::json document{{"schemaVersion", 1}, {"profiles", {minimalProfile()}}, {"systemChoices", choices}};
    return rv::launcher::parseProfileDocument(document, "emulators.json", origin);
}

} // namespace

TEST_CASE("launcher/system choices: a label or a profile id per system", "[launcher]") {
    const auto document = parse({{"snes", {{"command", "bsnes"}}}, {"gb", {{"profile", "mgba"}}}});
    CHECK(document.issues.empty());
    CHECK(document.profiles.size() == 1);
    REQUIRE(document.systemChoices.size() == 2);
    CHECK(document.systemChoices.at("snes") == LaunchChoice{LaunchChoiceKind::EsdeCommand, "bsnes"});
    CHECK(document.systemChoices.at("gb") == LaunchChoice{LaunchChoiceKind::Profile, "mgba"});
    CHECK(rv::launcher::toString(LaunchChoiceKind::EsdeCommand) == "command");
    CHECK(rv::launcher::toString(LaunchChoiceKind::Profile) == "profile");

    // Labels are kept as written (ES-DE compares them exactly): spaces, brackets, non-ASCII.
    const auto labels = parse({{"apple2", {{"command", "MAME - Current"}}}, {"psx", {{"command", "Bêta [Test] "}}}});
    CHECK(labels.issues.empty());
    CHECK(labels.systemChoices.at("psx").value == "Bêta [Test] ");
}

TEST_CASE("launcher/system choices: each invalid choice is an issue, the others are kept", "[launcher]") {
    const auto document = parse({{"gb", {{"profile", "mgba"}}},
                                 {"bad:name", {{"command", "x"}}},
                                 {"nes", {{"command", ""}}},
                                 {"snes", {{"profile", "Not An Id"}}},
                                 {"psx", {{"emulator", "x"}}},
                                 {"n64", {{"command", "a"}, {"profile", "b"}}},
                                 {"gba", "mgba"},
                                 {"gbc", {{"command", 5}}}});
    REQUIRE(document.systemChoices.size() == 1);
    CHECK(document.systemChoices.count("gb") == 1);
    REQUIRE(document.issues.size() == 7);
    const auto contextOf = [&](std::size_t i) { return document.issues[i].error.context; };
    // nlohmann::json orders an object's keys: bad:name, gba, gbc, n64, nes, psx, snes.
    CHECK(contextOf(0) == "emulators.json:/systemChoices/bad:name");
    CHECK(document.issues[0].error.code == rv::ErrorCode::ValidationFailed);
    CHECK(contextOf(1) == "emulators.json:/systemChoices/gba");
    CHECK(contextOf(2) == "emulators.json:/systemChoices/gbc/command");
    CHECK(contextOf(3) == "emulators.json:/systemChoices/n64");
    CHECK(contextOf(4) == "emulators.json:/systemChoices/nes/command");
    CHECK(document.issues[4].error.message == "an empty command label");
    CHECK(contextOf(5) == "emulators.json:/systemChoices/psx/emulator");
    CHECK(contextOf(6) == "emulators.json:/systemChoices/snes/profile");
    for (const auto& issue : document.issues) {
        CHECK(issue.profileId.empty());
    }

    const nlohmann::json notAnObject{{"schemaVersion", 1}, {"profiles", {minimalProfile()}}, {"systemChoices", 3}};
    const auto wrong = rv::launcher::parseProfileDocument(notAnObject, "emulators.json", ProfileOrigin::User);
    REQUIRE(wrong.issues.size() == 1);
    CHECK(wrong.issues[0].error.message == "expected an object of systems");
    CHECK(wrong.profiles.size() == 1);
    // A document without profiles gives no choice either.
    const nlohmann::json noProfiles{{"schemaVersion", 1}, {"systemChoices", {{"gb", {{"profile", "mgba"}}}}}};
    CHECK(rv::launcher::parseProfileDocument(noProfiles, "emulators.json", ProfileOrigin::User).systemChoices.empty());
}

TEST_CASE("launcher/system choices: the user's choice replaces the shipped one's per system", "[launcher]") {
    auto shipped = parse({{"gb", {{"command", "Shipped GB"}}}, {"nes", {{"command", "Shipped NES"}}}},
                         ProfileOrigin::Shipped);
    auto user = parse({{"gb", {{"profile", "emu"}}}, {"snes", {{"command", "bsnes"}}}});
    const rv::launcher::EmulatorProfiles merged =
        rv::launcher::mergeProfileDocuments(std::move(shipped), std::move(user));
    REQUIRE(merged.systemChoices.size() == 3);
    REQUIRE(merged.systemChoice("gb") != nullptr);
    CHECK(*merged.systemChoice("gb") == LaunchChoice{LaunchChoiceKind::Profile, "emu"});
    CHECK(merged.systemChoice("nes")->value == "Shipped NES");
    CHECK(merged.systemChoice("snes")->value == "bsnes");
    CHECK(merged.systemChoice("psx") == nullptr);
    CHECK(merged.systemChoice("GB") == nullptr); // system names compare exactly
}
