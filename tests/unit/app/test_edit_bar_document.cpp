// The contract between the editor's bar document (assets/ui/edit_bar.rml and .rcss; I4b step 3, ADR-0015 §4) and its
// code (rv::app::EditBarPanel, whose GPU side the unit tests cannot load: they share rv/app/edit_bar_ui.hpp): the ids,
// the data model and its bindings, a line for every mode, a sentence for every refusal it words, the menu's four buttons
// in order, the glyphs of both devices, and no colour transition. And the directory's "Modifica stanza".

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

#include <rv/app/edit_bar_ui.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/ui/directory_model.hpp>

namespace eb = rv::app::edit_bar;

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

TEST_CASE("app/edit_bar_document: the ids, the data model, every mode, refusal and menu button", "[app][documents]") {
    const std::string rml = readAsset(eb::kDocumentPath);
    CHECK(contains(rml, "data-model=\"" + std::string(eb::kDataModel) + "\""));
    const std::string sheet = rv::core::pathToUtf8(rv::core::pathFromUtf8(eb::kStyleSheetPath).filename());
    CHECK(contains(rml, "href=\"" + sheet + "\""));
    CHECK(contains(rml, "id=\"" + std::string(eb::kBarId) + "\""));
    CHECK(contains(rml, "id=\"" + std::string(eb::kMenuId) + "\""));
    // Every mode has its line (the menu's own panel shows in "menu").
    for (const std::string_view mode : eb::kModes) {
        INFO(mode);
        CHECK(contains(rml, "mode == '" + std::string(mode) + "'"));
    }
    // Every refusal it words has its sentence; another reads in general.
    for (const std::string_view rule : eb::kRules) {
        INFO(rule);
        CHECK(contains(rml, "data-if=\"refusal == '" + std::string(rule) + "'\""));
    }
    CHECK(contains(rml, "data-if=\"!refusalKnown\""));
    CHECK(contains(rml, "data-if=\"saveRefused\""));
    // The menu's buttons in order: Salva, Annulla ultima azione, Esci senza salvare, Continua.
    std::size_t at = 0;
    const std::string_view labels[] = {"Salva", "Annulla ultima azione", "Esci senza salvare", "Continua"};
    for (std::size_t index = 0; index < eb::kMenuButtonIds.size(); ++index) {
        const std::size_t found = rml.find("id=\"" + std::string(eb::kMenuButtonIds[index]) + "\"", at);
        INFO(eb::kMenuButtonIds[index]);
        REQUIRE(found != std::string::npos);
        CHECK(rml.find(labels[index], found) != std::string::npos);
        at = found;
    }
    for (const std::string_view focus : {"save", "undo", "discard", "continue"}) {
        CHECK(contains(rml, "menuFocus == '" + std::string(focus) + "'"));
    }
    // The glyphs of both devices; the bindings the panel gives.
    for (const std::string_view device : eb::kDevices) {
        CHECK(contains(rml, "device == '" + std::string(device) + "'"));
    }
    for (const std::string_view binding : {"{{ roomTitle }}", "{{ targetTitle }}", "{{ heldTitle }}", "{{ pending }}",
                                           "targetPinned", "targetFaceOut", "targetFree", "targetOwn", "ending"}) {
        INFO(binding);
        CHECK(contains(rml, binding));
    }
    // It takes no input: the world reads the editor's actions.
    CHECK_FALSE(contains(rml, "<button"));
    CHECK_FALSE(contains(rml, "<input"));
    CHECK_FALSE(contains(rml, "data-event"));

    const std::string rcss = readAsset(eb::kStyleSheetPath);
    CHECK(contains(rcss, "#" + std::string(eb::kBarId) + " {"));
    CHECK(contains(rcss, "#" + std::string(eb::kMenuId) + " {"));
    CHECK(contains(rcss, "position: absolute;"));
    CHECK_FALSE(contains(rcss, "transition:"));
}

TEST_CASE("app/edit_bar_document: every piece of furniture named, held and targeted", "[app][documents]") {
    const std::string rml = readAsset(eb::kDocumentPath);
    // Each kind the bar names has its noun twice: the target's title and the held one's "Stai spostando: ...".
    for (const std::string_view kind : eb::kFixtureKinds) {
        INFO(kind);
        const std::string binding = "data-if=\"fixtureKind == '" + std::string(kind) + "'\"";
        const std::size_t first = rml.find(binding);
        REQUIRE(first != std::string::npos);
        CHECK(rml.find(binding, first + 1) != std::string::npos);
    }
    CHECK(contains(rml, "Stai spostando:"));
    // Every prefab kind's name is one the document words; another kind is "furniture" ("un arredo").
    for (const auto& [prefab, name] : eb::kFixtureKindNames) {
        INFO(prefab);
        CHECK(eb::fixtureKindOf(prefab) == name);
        CHECK(std::find(eb::kFixtureKinds.begin(), eb::kFixtureKinds.end(), name) != eb::kFixtureKinds.end());
    }
    CHECK(eb::fixtureKindOf("banker_lamp") == "table-lamp");
    CHECK(eb::fixtureKindOf("crt") == "furniture");
    CHECK(eb::kFixtureKinds.back() == "furniture");
    // A fixture that cannot move names why at once, without "Prendi".
    CHECK(contains(rml, "!fixtureLocked && !fixtureHung"));
    CHECK(contains(rml, "target != 'dressed' && !fixtureLocked\"><span class=\"key pad-a\""));
}

TEST_CASE("app/edit_bar_document: the directory's \"Modifica stanza\"", "[app][documents]") {
    const std::string rml = readAsset(rv::ui::directory::kDocumentPath);
    CHECK(contains(rml, "id=\"" + std::string(rv::ui::directory::kEditRoomId) + "\""));
    CHECK(contains(rml, "data-event-click=\"" + std::string(rv::ui::directory::kEditEvent) + "()\""));
    CHECK(contains(rml, "Modifica stanza"));
    for (const std::string_view kind : rv::ui::directory::kRoomKinds) {
        CHECK(contains(rml, "editKind == '" + std::string(kind) + "'"));
    }
}
