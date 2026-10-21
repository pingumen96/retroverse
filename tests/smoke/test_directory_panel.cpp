// The directory (I4 step 7a; ADR-0013 §10) through rv_ui_probe (tests/ui_probe): the panel over the test room from a
// fixed model (directory_fixture.hpp: six floors, twelve favourites and recent games), driven by
// tests/input_scripts/directory_panel.json's UI actions through the action map and NavInput, hidden, lockstep, at
// 1920 x 1080. The checks read the probe's report and screenshots: at every checkpoint the model's state and the
// element that has RmlUi's focus are the fixture's (the floors, gen4's rooms, the floors again, the favourites and
// recent games, a game's card with Mostra nella stanza), the focused element lies inside its panel and shows the
// focus's tungsten edge, the card is never cut, the run ends with the one command the script leads to (Mostra nella
// stanza) and the panel closed, RmlUi logged no error. The screenshots (floors.png, rooms.png, games.png, card.png in
// the case's folder) are for the eye: Italian, legible, both tabs, the long names (gen4's shared room's five long
// systems wrapped after its count, the long title selected and ending in an ellipsis, a favourite's long system name
// after its date).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/assets/image_io.hpp>
#include <rv/core/json.hpp>

#include "directory_fixture.hpp"
#include "image_checks.hpp"
#include "smoke_support.hpp"

using namespace rv::smoke;
namespace fixture = rv::probe::directory_fixture;

namespace {

constexpr glm::ivec2 kShotSize{1920, 1080};
/// The focus's edge, #FFC88A (directory.rcss: .row:focus, .action:focus), drawn over the scene in sRGB.
constexpr glm::ivec3 kFocusEdge{0xFF, 0xC8, 0x8A};

std::filesystem::path probeExe() {
    return std::filesystem::path(RV_SMOKE_UI_PROBE);
}

std::optional<glm::vec4> boxOf(const nlohmann::json& value) {
    if (!value.is_array() || value.size() != 4) {
        return std::nullopt;
    }
    return glm::vec4(value[0].get<float>(), value[1].get<float>(), value[2].get<float>(), value[3].get<float>());
}

bool inside(const glm::vec4& inner, const glm::vec4& outer) {
    return inner.x >= outer.x - 0.5f && inner.y >= outer.y - 0.5f && inner.x + inner.z <= outer.x + outer.z + 0.5f &&
           inner.y + inner.w <= outer.y + outer.w + 0.5f;
}

glm::ivec3 pixelAt(const rv::assets::ImageData& image, int x, int y) {
    const std::size_t offset =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) *
        static_cast<std::size_t>(image.channels);
    return {image.pixels[offset], image.pixels[offset + 1], image.pixels[offset + 2]};
}

int distance(const glm::ivec3& a, const glm::ivec3& b) {
    return std::max({std::abs(a.x - b.x), std::abs(a.y - b.y), std::abs(a.z - b.z)});
}

} // namespace

TEST_CASE("directory: the panel's floors, rooms, games and card driven by the UI actions", "[directory]") {
    const std::filesystem::path caseDir = freshCaseDir("directory_panel");
    // Beside the run report (probe.json).
    const std::filesystem::path report = caseDir / "directory.json";
    const AppRun run = runApp(probeExe(), caseDir, "probe",
                              {"--input-script", arg(sourceDir() / "tests" / "input_scripts" / "directory_panel.json"),
                               "--probe-shots", arg(caseDir), "--probe-report", arg(report), "--exit-after-frames",
                               "--frames", std::to_string(fixture::kFrames), "--fixed-dt", "0.016667", "--width",
                               "1920", "--height", "1080"});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    const auto json = rv::core::readJsonFile(report);
    INFO(run.describe());
    REQUIRE(json.has_value());
    const nlohmann::json& probe = *json;

    // Every checkpoint as the fixture says, RmlUi's focus on the model's element.
    const nlohmann::json& checkpoints = probe.at("checkpoints");
    REQUIRE(checkpoints.size() == fixture::kCheckpoints.size());
    for (std::size_t index = 0; index < fixture::kCheckpoints.size(); ++index) {
        const fixture::Checkpoint& expected = fixture::kCheckpoints[index];
        const nlohmann::json& actual = checkpoints[index];
        INFO("frame " << expected.frame << ": " << actual.dump());
        CHECK(actual.at("frame").get<std::uint32_t>() == expected.frame);
        CHECK(actual.at("open").get<bool>() == expected.open);
        CHECK(actual.at("visible").get<bool>() == expected.open);
        CHECK_FALSE(actual.at("cardSqueezed").get<bool>());
        if (!expected.open) {
            CHECK(actual.at("screenFocus").get<std::string>().empty());
            continue;
        }
        CHECK(actual.at("tab").get<std::string>() == rv::ui::toString(expected.tab));
        CHECK(actual.at("view").get<std::string>() == rv::ui::toString(expected.view));
        CHECK(actual.at("modelFocus").get<std::string>() == expected.focus);
        CHECK(actual.at("screenFocus").get<std::string>() == expected.focus);
        // The focused element lies within its panel: a row within the list (scrolled into view), a card button within
        // the card.
        const std::optional<glm::vec4> focus = boxOf(actual.at("focusBox"));
        const bool card = expected.view == rv::ui::DirectoryView::Card;
        const std::optional<glm::vec4> panel = boxOf(actual.at(card ? "cardBox" : "listBox"));
        REQUIRE(focus.has_value());
        REQUIRE(panel.has_value());
        CHECK(inside(*focus, *panel));
    }

    // The one command: Mostra nella stanza on the third game, which closed the panel.
    const nlohmann::json& commands = probe.at("commands");
    REQUIRE(commands.size() == 1);
    CHECK(commands[0].at("frame").get<std::uint32_t>() == fixture::kShowInRoomFrame);
    CHECK(commands[0].at("kind").get<std::string>() == "show-in-room");
    CHECK(commands[0].at("id").get<std::string>() == fixture::gameId(fixture::kShownGame));
    CHECK(probe.at("rmlErrors").get<std::uint64_t>() == 0);

    // The screenshots (the probe's, not the run report's): written, not blank, the focused element's left edge in the
    // focus's tungsten.
    const nlohmann::json& written = probe.at("shots");
    std::size_t shots = 0;
    for (std::size_t index = 0; index < fixture::kCheckpoints.size(); ++index) {
        const fixture::Checkpoint& expected = fixture::kCheckpoints[index];
        if (expected.shot.empty()) {
            continue;
        }
        INFO(expected.shot);
        REQUIRE(shots < written.size());
        CHECK(written[shots].at("frame").get<std::uint32_t>() == expected.frame);
        CHECK(written[shots].at("written").get<bool>());
        ++shots;
        auto loaded = rv::assets::loadImage(caseDir / std::string(expected.shot), 3);
        REQUIRE(loaded.has_value());
        const rv::assets::ImageData& image = *loaded;
        REQUIRE(image.width == kShotSize.x);
        REQUIRE(image.height == kShotSize.y);
        const LumaSummary luma = lumaSummary(image);
        CHECK(luma.maximum - luma.minimum >= 32.0); // not uniform
        CHECK(luma.mean >= 4.0);                    // not black
        const std::optional<glm::vec4> focus = boxOf(checkpoints[index].at("focusBox"));
        REQUIRE(focus.has_value());
        const int y = static_cast<int>(std::lround(focus->y + focus->w / 2.0f));
        // Within the border's width (3 dp, 6 dp on the rows' left), from its outer edge.
        const glm::ivec3 edge = pixelAt(image, static_cast<int>(std::lround(focus->x)) + 1, y);
        CHECK(distance(edge, kFocusEdge) <= 8);
    }
    CHECK(shots == 4);
    CHECK(written.size() == shots);
}
