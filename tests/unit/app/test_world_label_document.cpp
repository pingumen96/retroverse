// The contract between the hover's title label document (assets/ui/world_label.rml and .rcss) and its code
// (rv::app::WorldLabelPanel, whose GPU side the unit tests cannot load: they share rv/app/world_label_ui.hpp): the ids,
// the data model and its bindings, the words for a game without a title or without its file, and no colour transition.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>

#include <rv/app/world_label_ui.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>

namespace wl = rv::app::world_label;

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

TEST_CASE("app/world_label_document: the ids, the data model and the words around the title", "[app][documents]") {
    const std::string rml = readAsset(wl::kDocumentPath);
    CHECK(contains(rml, "data-model=\"" + std::string(wl::kDataModel) + "\""));
    const std::string sheet = rv::core::pathToUtf8(rv::core::pathFromUtf8(wl::kStyleSheetPath).filename());
    CHECK(contains(rml, "href=\"" + sheet + "\""));
    CHECK(contains(rml, "id=\"" + std::string(wl::kLabelId) + "\""));
    // The panel binds the title, the availability and the band's position; the document reads each of them.
    CHECK(contains(rml, "{{ title }}"));
    CHECK(contains(rml, "data-if=\"title == ''\""));
    CHECK(contains(rml, "data-if=\"!available\""));
    for (const std::string_view binding : {"data-style-left=\"left\"", "data-style-bottom=\"bottom\"",
                                           "data-style-width=\"width\""}) {
        INFO(binding);
        CHECK(contains(rml, binding));
    }
    // It takes no input: nothing in it can be focused or clicked.
    CHECK_FALSE(contains(rml, "<button"));
    CHECK_FALSE(contains(rml, "<input"));
    CHECK_FALSE(contains(rml, "data-event"));

    const std::string rcss = readAsset(wl::kStyleSheetPath);
    CHECK(contains(rcss, "#" + std::string(wl::kLabelId) + " {"));
    CHECK(contains(rcss, "position: absolute;"));
    CHECK_FALSE(contains(rcss, "transition:"));
}
