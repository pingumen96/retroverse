// The world's game card (I3 step 14a; ADR-0012 §5): its model (the buttons' focus order in the 2 x 2 grid, the
// commands and clicks that press them, Avvia focused for every new game), and the contract between its document
// (assets/ui/game_card.rml and .rcss) and its screen (rv::ui::GameCardScreen, whose GPU side the unit tests cannot
// load: they share rv/ui/game_card_model.hpp): the ids, the data model and its bindings, the cover's rv-texture source,
// a press event per button, every readiness, blocker and outcome key, no colour transition; and the readiness and
// blocker sentences equal to the quick catalog's, line for line (one table).

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <map>
#include <regex>
#include <string>
#include <string_view>

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/ui/game_card_model.hpp>
#include <rv/ui/ui_assets.hpp>

namespace gc = rv::ui::game_card;
namespace qc = rv::ui::quick_catalog;
using rv::ui::CardButton;
using rv::ui::GameCardModel;
using rv::ui::NavDirection;
using rv::ui::UiCommand;

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

/// The sentences of a document's paragraphs whose condition names `key` ("card.readiness", "card.blocker"), by their
/// condition: data-if="<condition>">sentence</p>.
std::map<std::string, std::string> sentencesOf(const std::string& rml, std::string_view key) {
    std::map<std::string, std::string> sentences;
    static const std::regex paragraph(R"re(<p [^>]*data-if="([^"]*)"[^>]*>([^<]*)</p>)re");
    for (auto it = std::sregex_iterator(rml.begin(), rml.end(), paragraph); it != std::sregex_iterator(); ++it) {
        const std::string condition = (*it)[1].str();
        if (condition.find(key) != std::string::npos) {
            sentences.emplace(condition, (*it)[2].str());
        }
    }
    return sentences;
}

} // namespace

TEST_CASE("ui/game_card: the buttons' focus moves within the 2 x 2 grid without wrapping", "[ui][game_card]") {
    // Avvia, Vedi media / Informazioni, Indietro.
    CHECK(rv::ui::stepFocus(CardButton::Play, NavDirection::Right) == CardButton::Media);
    CHECK(rv::ui::stepFocus(CardButton::Play, NavDirection::Down) == CardButton::Info);
    CHECK(rv::ui::stepFocus(CardButton::Play, NavDirection::Left) == CardButton::Play);
    CHECK(rv::ui::stepFocus(CardButton::Play, NavDirection::Up) == CardButton::Play);
    CHECK(rv::ui::stepFocus(CardButton::Media, NavDirection::Down) == CardButton::Back);
    CHECK(rv::ui::stepFocus(CardButton::Media, NavDirection::Left) == CardButton::Play);
    CHECK(rv::ui::stepFocus(CardButton::Media, NavDirection::Right) == CardButton::Media);
    CHECK(rv::ui::stepFocus(CardButton::Info, NavDirection::Right) == CardButton::Back);
    CHECK(rv::ui::stepFocus(CardButton::Info, NavDirection::Up) == CardButton::Play);
    CHECK(rv::ui::stepFocus(CardButton::Info, NavDirection::Down) == CardButton::Info);
    CHECK(rv::ui::stepFocus(CardButton::Back, NavDirection::Up) == CardButton::Media);
    CHECK(rv::ui::stepFocus(CardButton::Back, NavDirection::Left) == CardButton::Info);
    CHECK(rv::ui::stepFocus(CardButton::Back, NavDirection::Down) == CardButton::Back);
    // Every button of the grid is reachable from every other in at most two steps (Sposta does not show).
    const CardButton grid[] = {CardButton::Play, CardButton::Media, CardButton::Info, CardButton::Back};
    for (const NavDirection direction :
         {NavDirection::Up, NavDirection::Down, NavDirection::Left, NavDirection::Right}) {
        for (const CardButton from : grid) {
            CHECK(rv::ui::stepFocus(from, direction) != CardButton::Move);
        }
    }
    for (const CardButton from : grid) {
        for (const CardButton to : grid) {
            bool reached = from == to;
            for (const NavDirection first :
                 {NavDirection::Up, NavDirection::Down, NavDirection::Left, NavDirection::Right}) {
                const CardButton one = rv::ui::stepFocus(from, first);
                reached = reached || one == to;
                for (const NavDirection second :
                     {NavDirection::Up, NavDirection::Down, NavDirection::Left, NavDirection::Right}) {
                    reached = reached || rv::ui::stepFocus(one, second) == to;
                }
            }
            INFO(rv::ui::toString(from) << " -> " << rv::ui::toString(to));
            CHECK(reached);
        }
    }
}

TEST_CASE("ui/game_card: \"Sposta\" is a row of its own below the grid while the game's room can be edited",
          "[ui][game_card]") {
    // I4b step 3: Down from the second row, Up back to Informazioni, the other ways stay.
    CHECK(rv::ui::stepFocus(CardButton::Info, NavDirection::Down, true) == CardButton::Move);
    CHECK(rv::ui::stepFocus(CardButton::Back, NavDirection::Down, true) == CardButton::Move);
    CHECK(rv::ui::stepFocus(CardButton::Play, NavDirection::Down, true) == CardButton::Info);
    CHECK(rv::ui::stepFocus(CardButton::Move, NavDirection::Up, true) == CardButton::Info);
    CHECK(rv::ui::stepFocus(CardButton::Move, NavDirection::Down, true) == CardButton::Move);
    CHECK(rv::ui::stepFocus(CardButton::Move, NavDirection::Left, true) == CardButton::Move);
    CHECK(rv::ui::stepFocus(CardButton::Move, NavDirection::Right, true) == CardButton::Move);
    // The model: Sposta reached and pressed while movable; gone with the focus on it, the focus goes to Indietro.
    GameCardModel model;
    model.show("g0000000000000001", true);
    static_cast<void>(model.apply(UiCommand::Down));
    static_cast<void>(model.apply(UiCommand::Down));
    CHECK(model.focused() == CardButton::Move);
    CHECK(model.apply(UiCommand::Activate) == CardButton::Move);
    model.show("g0000000000000001", false);
    CHECK(model.focused() == CardButton::Back);
    static_cast<void>(model.apply(UiCommand::Down));
    CHECK(model.focused() == CardButton::Back);
    // Another game starts on Avvia, movable or not.
    model.show("g0000000000000002", true);
    CHECK(model.focused() == CardButton::Play);
}

TEST_CASE("ui/game_card: the model presses the focused button, its shortcuts and the clicked one", "[ui][game_card]") {
    GameCardModel model;
    CHECK_FALSE(model.shown());
    model.show("g0000000000000001");
    CHECK(model.focused() == CardButton::Play);
    // The directions move the focus and press nothing; Activate presses the focused button.
    CHECK_FALSE(model.apply(UiCommand::Right).has_value());
    CHECK(model.focused() == CardButton::Media);
    CHECK(model.apply(UiCommand::Activate) == CardButton::Media);
    CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    CHECK(model.apply(UiCommand::Activate) == CardButton::Back);
    CHECK_FALSE(model.apply(UiCommand::Left).has_value());
    CHECK(model.apply(UiCommand::Activate) == CardButton::Info);
    CHECK_FALSE(model.apply(UiCommand::Up).has_value());
    CHECK(model.apply(UiCommand::Activate) == CardButton::Play);
    // The shortcuts press their button without moving the focus.
    CHECK(model.apply(UiCommand::Media) == CardButton::Media);
    CHECK(model.apply(UiCommand::Info) == CardButton::Info);
    CHECK(model.apply(UiCommand::Back) == CardButton::Back);
    CHECK(model.focused() == CardButton::Play);
    CHECK_FALSE(model.apply(UiCommand::PageDown).has_value());
    // A click focuses and presses.
    CHECK(model.click(CardButton::Info) == CardButton::Info);
    CHECK(model.focused() == CardButton::Info);
    // The same game keeps its focus; another game starts on Avvia; so does the card shown again.
    model.show("g0000000000000001");
    CHECK(model.focused() == CardButton::Info);
    model.show("g0000000000000002");
    CHECK(model.focused() == CardButton::Play);
    model.apply(UiCommand::Right);
    model.hide();
    CHECK_FALSE(model.shown());
    model.show("g0000000000000002");
    CHECK(model.focused() == CardButton::Play);
}

TEST_CASE("ui/game_card: button names and element ids", "[ui][game_card]") {
    CHECK(rv::ui::toString(CardButton::Play) == "play");
    CHECK(rv::ui::toString(CardButton::Media) == "media");
    CHECK(rv::ui::toString(CardButton::Info) == "info");
    CHECK(rv::ui::toString(CardButton::Back) == "back");
    for (const CardButton button : rv::ui::kCardButtons) {
        CHECK(rv::ui::cardButtonFromName(rv::ui::toString(button)) == button);
    }
    CHECK_FALSE(rv::ui::cardButtonFromName("Play").has_value());
    CHECK_FALSE(rv::ui::cardButtonFromName("").has_value());
    CHECK(rv::ui::elementIdOf(CardButton::Play) == gc::kPlayId);
    CHECK(rv::ui::elementIdOf(CardButton::Back) == gc::kBackId);
    CHECK(rv::ui::toString(CardButton::Move) == "move");
    CHECK(rv::ui::elementIdOf(CardButton::Move) == gc::kMoveId);
    CHECK(gc::kCoverSource == std::string(gc::kTextureScheme) + std::string(gc::kCoverTexture));
}

TEST_CASE("ui/game_card: the cover stands in its frame at its own shape, never stretched", "[ui][game_card]") {
    using rv::ui::CardCoverBox;
    using rv::ui::cardCoverBox;
    // The frame's own shape (the 16-bit cartridges', I3's card) and none fill it.
    CHECK(cardCoverBox(0, 0) == CardCoverBox{});
    CHECK(cardCoverBox(256, 188) == CardCoverBox{});
    CHECK(cardCoverBox(64, 47) == CardCoverBox{});
    // A jewel case's square cover: the frame's height, pillarboxed and centred.
    const CardCoverBox square = cardCoverBox(120, 120);
    CHECK(square.height == 1.0);
    CHECK(square.width == 188.0 / 256.0);
    CHECK(square.left == (1.0 - 188.0 / 256.0) / 2.0);
    CHECK(square.top == 0.0);
    // A keep case's portrait one, and a box's.
    const CardCoverBox keepCase = cardCoverBox(123, 178);
    CHECK(keepCase.height == 1.0);
    CHECK(std::abs(keepCase.width * 256.0 / 188.0 - 123.0 / 178.0) < 1e-12);
    CHECK(std::abs(keepCase.left * 2.0 + keepCase.width - 1.0) < 1e-12);
    // A shape wider than the frame (the 64-bit cartridge's 96 x 60): the frame's width, letterboxed.
    const CardCoverBox wide = cardCoverBox(96, 60);
    CHECK(wide.width == 1.0);
    CHECK(wide.left == 0.0);
    CHECK(std::abs(256.0 / (wide.height * 188.0) - 96.0 / 60.0) < 1e-12);
    CHECK(std::abs(wide.top * 2.0 + wide.height - 1.0) < 1e-12);
}

TEST_CASE("ui/game_card: the document has the ids, bindings, buttons and keys its screen uses",
          "[ui][game_card][documents]") {
    const std::string rml = readAsset(gc::kDocumentPath);
    CHECK(contains(rml, "data-model=\"" + std::string(gc::kDataModel) + "\""));
    const std::string sheet = rv::core::pathToUtf8(rv::core::pathFromUtf8(gc::kStyleSheetPath).filename());
    CHECK(contains(rml, "href=\"" + sheet + "\""));
    for (const std::string_view id : {gc::kCardId, gc::kTitleId, gc::kCoverId, gc::kActionsId}) {
        INFO(id);
        CHECK(contains(rml, "id=\"" + std::string(id) + "\""));
    }
    // The cover is the application's texture, never a file.
    CHECK(contains(rml, "src=\"" + std::string(gc::kCoverSource) + "\""));
    CHECK_FALSE(contains(rml, ".png"));
    CHECK_FALSE(contains(rml, ".jpg"));
    // A button per action, in focus order, each pressing its own name; their words.
    std::size_t last = 0;
    for (const CardButton button : rv::ui::kCardButtons) {
        const std::string tag = "<button id=\"" + std::string(rv::ui::elementIdOf(button)) + "\"";
        const std::size_t at = rml.find(tag);
        INFO(rv::ui::toString(button));
        REQUIRE(at != std::string::npos);
        CHECK(at > last);
        last = at;
        CHECK(contains(rml, "data-event-click=\"" + std::string(gc::kPressEvent) + "('" +
                                std::string(rv::ui::toString(button)) + "')\""));
    }
    for (const std::string_view word : {">Avvia<", ">Vedi media<", ">Informazioni<", ">Indietro<", ">Sposta<"}) {
        INFO(word);
        CHECK(contains(rml, word));
    }
    // The bindings the screen fills.
    for (const std::string_view binding :
         {"{{ card.title }}", "{{ card.platform }}", "{{ card.year }}", "{{ card.developer }}", "{{ card.genre }}",
          "{{ card.players }}", "{{ card.otherVersions }}", "card.cover", "card.details", "card.unavailable",
          "card.verified", "card.outcomeKnown", "data-if=\"card.movable\"", "data-style-width=\"card.coverWidth\"",
          "data-style-height=\"card.coverHeight\"", "data-style-margin-left=\"card.coverLeft\"",
          "data-style-margin-top=\"card.coverTop\""}) {
        INFO(binding);
        CHECK(contains(rml, binding));
    }
    for (const std::string_view readiness : qc::kReadinessKeys) {
        INFO(readiness);
        CHECK(contains(rml, "card.readiness == '" + std::string(readiness) + "'"));
    }
    for (const std::string_view code : qc::kBlockerCodes) {
        INFO(code);
        CHECK(contains(rml, "card.blocker == '" + std::string(code) + "'"));
    }
    for (const std::string_view code : gc::kOutcomeCodes) {
        INFO(code);
        CHECK(contains(rml, "card.outcome == '" + std::string(code) + "'"));
    }
    // Every word in Italian is the document's: an empty field reads "n.d.", a game without a title has words of its
    // own.
    CHECK(contains(rml, "Gioco senza titolo"));
    CHECK(contains(rml, "n.d."));

    const std::string rcss = readAsset(gc::kStyleSheetPath);
    CHECK(contains(rcss, "#" + std::string(gc::kCoverId) + " {"));
    CHECK(contains(rcss, ".action:focus {"));
    CHECK(contains(rcss, "@media (theme: compact)"));
    CHECK_FALSE(contains(rcss, "transition"));
    CHECK_FALSE(contains(rcss, "px;"));
}

TEST_CASE("ui/game_card: the readiness and blocker sentences are the quick catalog's", "[ui][game_card][documents]") {
    const std::string card = readAsset(gc::kDocumentPath);
    const std::string catalog = readAsset(qc::kDocumentPath);
    for (const std::string_view key : {"card.readiness", "card.blocker"}) {
        const auto expected = sentencesOf(catalog, key);
        const auto actual = sentencesOf(card, key);
        // The quick catalog's every readiness label and blocker sentence (33 codes, a few sharing a sentence).
        CHECK(expected.size() >= (key == std::string_view("card.blocker") ? 31u : 6u));
        for (const auto& [condition, sentence] : expected) {
            INFO(condition);
            const auto found = actual.find(condition);
            REQUIRE(found != actual.end());
            CHECK(found->second == sentence);
        }
    }
}
