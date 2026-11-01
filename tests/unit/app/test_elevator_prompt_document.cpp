// The contract between the elevator's prompt document (assets/ui/elevator_prompt.rml and .rcss, I4 step 7b) and its
// code (rv::app::ElevatorPromptPanel, whose GPU side the unit tests cannot load: they share
// rv/app/elevator_prompt_ui.hpp): the ids, the data model and its binding, the Italian words, no input, no colour
// transition.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>

#include <rv/app/elevator_prompt_ui.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>

namespace ep = rv::app::elevator_prompt;

namespace {

std::string readAsset(std::string_view relative) {
    const std::filesystem::path assets = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    auto text = rv::core::readTextFile(assets / rv::core::pathFromUtf8(relative));
    REQUIRE(text.has_value());
    return *text;
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("app/elevator_prompt_document: the ids, the data model and the words", "[app][documents]") {
    const std::string rml = readAsset(ep::kDocumentPath);
    CHECK(contains(rml, "data-model=\"" + std::string(ep::kDataModel) + "\""));
    const std::string sheet = rv::core::pathToUtf8(rv::core::pathFromUtf8(ep::kStyleSheetPath).filename());
    CHECK(contains(rml, "href=\"" + sheet + "\""));
    CHECK(contains(rml, "id=\"" + std::string(ep::kPanelId) + "\""));
    CHECK(contains(rml, "{{ key }}"));
    CHECK(contains(rml, ">Ascensore<"));
    CHECK(contains(rml, ">Scegli il piano<"));
    // I6 step 5: the hub's directory totem's own words, by the model's "totem".
    CHECK(contains(rml, "data-if=\"totem\">Indice<"));
    CHECK(contains(rml, "data-if=\"!totem\">Ascensore<"));
    CHECK(contains(rml, "data-if=\"totem\">Piani e stanze<"));
    // It takes no input: nothing in it can be focused or clicked.
    CHECK_FALSE(contains(rml, "<button"));
    CHECK_FALSE(contains(rml, "<input"));
    CHECK_FALSE(contains(rml, "data-event"));

    const std::string rcss = readAsset(ep::kStyleSheetPath);
    CHECK(contains(rcss, "#" + std::string(ep::kPanelId) + " {"));
    CHECK(contains(rcss, "position: absolute;"));
    CHECK_FALSE(contains(rcss, "transition:"));
}

TEST_CASE("app/elevator_prompt_document: the totem's prompt shows to a walker in its zone that faces its screen",
          "[app][elevator_prompt]") {
    // The hub's totem as the floor build gives it (I6 step 5): its zone 1.5 m deep west of its screen, which faces west
    // at (3.44, 2.4).
    const rv::core::Aabb zone{glm::vec3(1.94f, 0.0f, 1.6f), glm::vec3(3.44f, 2.2f, 3.2f)};
    const glm::vec3 screen(3.44f, 0.0f, 2.4f);
    constexpr float kDegrees = 3.14159265f / 180.0f;
    CHECK(rv::app::facesTotem(glm::vec3(2.6f, 0.0f, 2.4f), 90.0f * kDegrees, zone, screen));   // facing east, at it
    CHECK_FALSE(rv::app::facesTotem(glm::vec3(2.6f, 0.0f, 2.4f), 270.0f * kDegrees, zone, screen)); // its back to it
    CHECK_FALSE(rv::app::facesTotem(glm::vec3(1.2f, 0.0f, 0.0f), 90.0f * kDegrees, zone, screen));  // the arrival
    CHECK_FALSE(rv::app::facesTotem(glm::vec3(2.6f, 0.0f, 2.4f), 90.0f * kDegrees, rv::core::Aabb{}, screen));
}

TEST_CASE("app/elevator_prompt_document: the prompt shows to a walker in the elevator's zone that faces its doors",
          "[app][elevator_prompt]") {
    // A floor's elevator as the floor build gives it (its frame: the hall's west end): the zone 1.5 m deep and 2 m wide
    // before the doors, whose middle is on the west wall; the arrival point 1.2 m east of it facing east (yaw 90).
    const rv::core::Aabb zone{glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.5f, 2.2f, 1.0f)};
    const glm::vec3 doors(0.025f, 0.0f, 0.0f);
    const glm::vec3 arrival(1.2f, 0.0f, 0.0f);
    constexpr float kDegrees = 3.14159265f / 180.0f;
    // At the arrival point facing the hall, the doors behind: no prompt; turned to them: the prompt.
    CHECK_FALSE(rv::app::facesElevator(arrival, 90.0f * kDegrees, zone, doors));
    CHECK(rv::app::facesElevator(arrival, -90.0f * kDegrees, zone, doors));
    CHECK(rv::app::facesElevator(arrival, -135.0f * kDegrees, zone, doors));
    CHECK(rv::app::facesElevator(arrival, -45.0f * kDegrees, zone, doors));
    // Looking along the wall (north or south), or anywhere behind it: no prompt; 75 degrees off the doors: the prompt.
    CHECK_FALSE(rv::app::facesElevator(arrival, 0.0f, zone, doors));
    CHECK_FALSE(rv::app::facesElevator(arrival, 180.0f * kDegrees, zone, doors));
    CHECK_FALSE(rv::app::facesElevator(arrival, -5.0f * kDegrees, zone, doors));
    CHECK(rv::app::facesElevator(arrival, -15.0f * kDegrees, zone, doors));
    // In a corner of the zone, the doors' middle ahead to the left: the prompt.
    CHECK(rv::app::facesElevator(glm::vec3(0.3f, 0.0f, 0.9f), -90.0f * kDegrees, zone, doors));
    // Facing the doors from outside the zone: none; the zone of a frame that has none: none.
    CHECK_FALSE(rv::app::facesElevator(glm::vec3(1.6f, 0.0f, 0.0f), -90.0f * kDegrees, zone, doors));
    CHECK_FALSE(rv::app::facesElevator(glm::vec3(1.2f, 0.0f, 1.1f), -90.0f * kDegrees, zone, doors));
    CHECK_FALSE(rv::app::facesElevator(arrival, -90.0f * kDegrees, rv::core::Aabb{}, doors));
    // A half-turned frame (a start room in the south row): the zone and the doors turned with it, east now -X.
    const rv::core::Aabb turned{glm::vec3(-1.5f, 0.0f, -1.0f), glm::vec3(0.0f, 2.2f, 1.0f)};
    CHECK(rv::app::facesElevator(-arrival, 90.0f * kDegrees, turned, -doors));
    CHECK_FALSE(rv::app::facesElevator(-arrival, -90.0f * kDegrees, turned, -doors));
}
