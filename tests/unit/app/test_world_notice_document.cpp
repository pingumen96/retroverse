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
    CHECK(contains(rml, "{{ system }}"));
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
