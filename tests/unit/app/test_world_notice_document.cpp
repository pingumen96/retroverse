// The contract between the world's notice document (assets/ui/world_notice.rml and .rcss) and its code
// (rv::app::WorldNoticePanel, whose GPU side the unit tests cannot load: they share rv/app/world_notice_ui.hpp): the
// ids, the data model, a sentence for every notice and every catalog state, and no user-facing string in C++ (the keys
// are the only link).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

#include <rv/app/catalog_worker.hpp>
#include <rv/app/world_notice_ui.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>

namespace wn = rv::app::world_notice;

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

TEST_CASE("app/world_notice_document: the ids, the data model and a sentence for every notice", "[app][documents]") {
    const std::string rml = readAsset(wn::kDocumentPath);
    CHECK(contains(rml, "data-model=\"" + std::string(wn::kDataModel) + "\""));
    const std::string sheet = rv::core::pathToUtf8(rv::core::pathFromUtf8(wn::kStyleSheetPath).filename());
    CHECK(contains(rml, "href=\"" + sheet + "\""));
    CHECK(contains(rml, "id=\"" + std::string(wn::kPanelId) + "\""));
    for (const std::string_view notice : wn::kNotices) {
        INFO(notice);
        CHECK(contains(rml, "data-if=\"notice == '" + std::string(notice) + "'\""));
    }
    for (const std::string_view state : wn::kCatalogStates) {
        INFO(state);
        CHECK(contains(rml, "data-if=\"state == '" + std::string(state) + "'\""));
    }
    CHECK(contains(rml, "{{ room }}"));
    CHECK_FALSE(contains(rml, "{{ system }}")); // the panel binds no "system" since the start room may be an area
    // The catalog's states the test room names are the catalog's own words for them.
    for (const rv::app::CatalogState state :
         {rv::app::CatalogState::NoLibrary, rv::app::CatalogState::NoDatabase, rv::app::CatalogState::DataDirectory,
          rv::app::CatalogState::LibraryMismatch, rv::app::CatalogState::Unavailable}) {
        CHECK(std::find(wn::kCatalogStates.begin(), wn::kCatalogStates.end(), rv::app::toString(state)) !=
              wn::kCatalogStates.end());
    }
    // The style sheet exists and places the band.
    const std::string rcss = readAsset(wn::kStyleSheetPath);
    CHECK(contains(rcss, "#" + std::string(wn::kPanelId) + " {"));
}

TEST_CASE("app/world_notice_document: the test room of a catalog without rooms speaks of the building, a start room "
          "only when --start-room names one",
          "[app][documents]") {
    // I4 step 5a: no-games is the building's (no system of the catalog gives a room); "room" is the --start-room room's
    // id (a console room's or an area's), empty in the default lobby start, so its sentence shows only with one (never
    // "()").
    const std::string rml = readAsset(wn::kDocumentPath);
    const std::string opening = "<div data-if=\"notice == '" + std::string(wn::kNoGames) + "'\">";
    const std::size_t begin = rml.find(opening);
    REQUIRE(begin != std::string::npos);
    const std::size_t end = rml.find("</div>", begin);
    REQUIRE(end != std::string::npos);
    const std::string block = rml.substr(begin, end - begin);
    CHECK(contains(block, "Nessun sistema del catalogo ha una stanza"));
    CHECK_FALSE(contains(block, "del sistema della stanza"));
    // Every {{ room }} of the block is inside the paragraph shown only when there is a start room.
    const std::string guarded = "<p data-if=\"room != ''\">";
    std::size_t at = block.find("{{ room }}");
    REQUIRE(at != std::string::npos);
    for (; at != std::string::npos; at = block.find("{{ room }}", at + 1)) {
        const std::size_t paragraph = block.rfind("<p", at);
        REQUIRE(paragraph != std::string::npos);
        CHECK(block.compare(paragraph, guarded.size(), guarded) == 0);
    }
}

TEST_CASE("app/world_notice_document: the rooms an update added are named with their number (I4b step 8)",
          "[app][documents]") {
    const std::string rml = readAsset(wn::kDocumentPath);
    const std::string opening = "<div data-if=\"notice == '" + std::string(wn::kRoomsAdded) + "'\">";
    const std::size_t begin = rml.find(opening);
    REQUIRE(begin != std::string::npos);
    const std::size_t end = rml.find("</div>", begin);
    REQUIRE(end != std::string::npos);
    const std::string block = rml.substr(begin, end - begin);
    // The first new room by its kind's name and its number in its chain, the others counted.
    CHECK(contains(block, "data-if=\"roomKind == 'console'\">{{ roomTitle }} {{ roomNumber }}</p>"));
    CHECK(contains(block, "Sala condivisa {{ roomNumber }}"));
    CHECK(contains(block, "Sala giochi arcade {{ roomNumber }}"));
    CHECK(contains(block, "data-if=\"moreRooms == 0\""));
    CHECK(contains(block, "data-if=\"moreRooms == 1\""));
    CHECK(contains(block, "{{ moreRooms }}"));
    CHECK(contains(block, "nell'indice dell'edificio (M)"));
}
