// The contract between the quick catalog's document (assets/ui/quick_catalog.rml and .rcss) and its screen
// (rv::ui::QuickCatalogScreen, whose GPU side the unit tests cannot load: they share rv/ui/ui_assets.hpp), and the
// registry records of the bundled fonts (style §11.1).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/ui/ui_assets.hpp>

namespace qc = rv::ui::quick_catalog;

namespace {

const std::filesystem::path kAssets = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);

std::string readAsset(std::string_view relative) {
    auto text = rv::core::readTextFile(kAssets / rv::core::pathFromUtf8(relative));
    REQUIRE(text.has_value());
    return *text;
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

/// The declarations of the first RCSS rule whose selector list is exactly `selector` ("" when there is none).
std::string ruleBody(const std::string& sheet, std::string_view selector) {
    const std::string opening = std::string(selector) + " {";
    for (std::size_t at = sheet.find(opening); at != std::string::npos; at = sheet.find(opening, at + 1)) {
        // A selector of its own: at the start of a line.
        if (at == 0 || sheet[at - 1] == '\n' || sheet[at - 1] == '\t') {
            const std::size_t open = at + opening.size();
            const std::size_t close = sheet.find('}', open);
            return close == std::string::npos ? std::string() : sheet.substr(open, close - open);
        }
    }
    return {};
}

nlohmann::json readRegistry(std::string_view file) {
    auto json = rv::core::readJsonFile(kAssets / "registry" / rv::core::pathFromUtf8(file));
    REQUIRE(json.has_value());
    return *json;
}

} // namespace

TEST_CASE("ui/documents: the quick catalog document has the ids, regions and bindings its screen uses",
          "[ui][documents]") {
    const std::string rml = readAsset(qc::kDocumentPath);
    CHECK(contains(rml, "data-model=\"" + std::string(qc::kDataModel) + "\""));
    // The style sheet next to the document.
    const std::string sheetName = rv::core::pathToUtf8(rv::core::pathFromUtf8(qc::kStyleSheetPath).filename());
    CHECK(contains(rml, "href=\"" + sheetName + "\""));
    for (const std::string_view id : {qc::kSearchId, qc::kPlayId, qc::kMediaId, qc::kInfoId}) {
        INFO(id);
        CHECK(contains(rml, "id=\"" + std::string(id) + "\""));
    }
    for (const std::string_view region : {qc::kRegionSearch, qc::kRegionChips, qc::kRegionList, qc::kRegionActions}) {
        INFO(region);
        CHECK(contains(rml, std::string(qc::kRegionAttribute) + "=\"" + std::string(region) + "\""));
    }
    // The data model's variables and events (quick_catalog_screen.cpp registers them).
    for (const std::string_view binding : {"chip : chips", "game : games", "card.present", "card.title", "status",
                                           "count", "data-value=\"search\"", "filter(chip.key)", "launch()",
                                           "media()", "info()", "chip.elementId", "game.elementId"}) {
        INFO(binding);
        CHECK(contains(rml, binding));
    }
    // The pad actions of style §16.3 in the hint bar, in Italian.
    for (const std::string_view hint : {"Avvia", "Vedi media", "Informazioni", "Indietro"}) {
        INFO(hint);
        CHECK(contains(rml, hint));
    }
}

TEST_CASE("ui/documents: focusable elements navigate spatially inside regions that confine them",
          "[ui][documents]") {
    const std::string rcss = readAsset(qc::kStyleSheetPath);
    // A region is a scroll container, which RmlUi's spatial navigation does not leave.
    CHECK(contains(ruleBody(rcss, ".region"), "overflow: hidden"));
    CHECK(contains(ruleBody(rcss, "#game-list"), "overflow-y: auto"));
    for (const std::string_view selector : {"#search", ".chip", ".game", ".action"}) {
        INFO(selector);
        const std::string body = ruleBody(rcss, selector);
        CHECK(contains(body, "tab-index: auto"));
        CHECK(contains(body, "nav: auto"));
    }
    // The focus is always visible.
    for (const std::string_view selector : {"#search:focus", ".chip:focus", ".game:focus", ".action:focus"}) {
        INFO(selector);
        CHECK(contains(ruleBody(rcss, selector), "border-color"));
    }
    CHECK(contains(rcss, "body." + std::string(qc::kHighContrastClass)));
}

TEST_CASE("ui/documents: every bundled font has a registry entry with its provenance and license", "[ui][documents]") {
    const nlohmann::json assets = readRegistry("assets.json");
    const nlohmann::json provenance = readRegistry("provenance.json");
    const auto entryFor = [&assets](const std::string& path) -> const nlohmann::json* {
        for (const nlohmann::json& entry : assets.at("assets")) {
            if (entry.at("path") == path) {
                return &entry;
            }
        }
        return nullptr;
    };
    const auto recordFor = [&provenance](const std::string& id) -> const nlohmann::json* {
        for (const nlohmann::json& record : provenance.at("records")) {
            if (record.at("id") == id) {
                return &record;
            }
        }
        return nullptr;
    };

    std::vector<std::string> files;
    for (const auto& file : std::filesystem::recursive_directory_iterator(kAssets / "ui" / "fonts")) {
        if (file.is_regular_file()) {
            files.push_back(rv::core::pathToGenericUtf8(std::filesystem::relative(file.path(), kAssets)));
        }
    }
    REQUIRE_FALSE(files.empty());
    for (const std::string& file : files) {
        INFO(file);
        const nlohmann::json* entry = entryFor(file);
        REQUIRE(entry != nullptr);
        CHECK(entry->at("kind") == "font");
        CHECK(entry->at("sha256").get<std::string>().size() == 64);
        const nlohmann::json* record = recordFor(entry->at("provenance").get<std::string>());
        REQUIRE(record != nullptr);
        if (!record->at("licenseSnapshot").is_null()) {
            CHECK(std::filesystem::is_regular_file(kAssets / "registry" /
                                                   record->at("licenseSnapshot").get<std::string>()));
        }
    }
}

TEST_CASE("ui/documents: the default font faces are bundled TrueType files under the OFL", "[ui][documents]") {
    for (const std::string_view face : rv::ui::kUiFontFaces) {
        INFO(face);
        auto bytes = rv::core::readBinaryFile(kAssets / rv::core::pathFromUtf8(face));
        REQUIRE(bytes.has_value());
        REQUIRE(bytes->size() > 4);
        // sfnt version 1.0: TrueType outlines.
        CHECK(std::to_integer<int>((*bytes)[0]) == 0x00);
        CHECK(std::to_integer<int>((*bytes)[1]) == 0x01);
        CHECK(std::to_integer<int>((*bytes)[2]) == 0x00);
        CHECK(std::to_integer<int>((*bytes)[3]) == 0x00);
    }
    const std::string license = readAsset("registry/licenses/OFL-1.1-AtkinsonHyperlegible.txt");
    CHECK(contains(license, "SIL OPEN FONT LICENSE Version 1.1"));
    CHECK(contains(license, "Braille Institute of America"));
}
