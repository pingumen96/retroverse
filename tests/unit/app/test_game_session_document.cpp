// The contract between the game-session panel's document (assets/ui/game_session.rml and .rcss) and its code
// (rv::app::GameSessionPanel, whose GPU side the unit tests cannot load: they share rv/app/game_session_ui.hpp and the
// codes of rv/app/launch_outcome.hpp): the ids, the data model, a text for every phase, outcome code and reason, and no
// user-facing string in C++ (the codes are the only link).

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>

#include <rv/app/game_session_ui.hpp>
#include <rv/app/launch_flow.hpp>
#include <rv/app/launch_outcome.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>

namespace gs = rv::app::game_session_ui;
namespace lo = rv::app::launch_outcome;

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

TEST_CASE("app/game_session_document: the ids, the data model and the events its panel binds",
          "[app][documents]") {
    const std::string rml = readAsset(gs::kDocumentPath);
    CHECK(contains(rml, "data-model=\"" + std::string(gs::kDataModel) + "\""));
    const std::string sheet = rv::core::pathToUtf8(rv::core::pathFromUtf8(gs::kStyleSheetPath).filename());
    CHECK(contains(rml, "href=\"" + sheet + "\""));
    for (const std::string_view id : {gs::kPanelId, gs::kReturnId, gs::kResumeId, gs::kOkId}) {
        INFO(id);
        CHECK(contains(rml, "id=\"" + std::string(id) + "\""));
    }
    for (const std::string_view binding : {"{{ title }}", "{{ detail }}", "{{ timeout }}", "data-event-click=\"return_home()\"",
                                           "data-event-click=\"resume()\"", "data-event-click=\"dismiss()\""}) {
        INFO(binding);
        CHECK(contains(rml, binding));
    }
    // The style sheet exists and makes the choices focusable by navigation.
    const std::string rcss = readAsset(gs::kStyleSheetPath);
    CHECK(contains(rcss, ".choice {"));
    CHECK(contains(rcss, "nav: auto;"));
    CHECK(contains(rcss, "tab-index: auto;"));
    CHECK(contains(rcss, ".choice:focus"));
}

TEST_CASE("app/game_session_document: a text for every phase, outcome code and reason", "[app][documents]") {
    const std::string rml = readAsset(gs::kDocumentPath);
    using rv::app::SessionPanel;
    for (const SessionPanel panel : {SessionPanel::Launching, SessionPanel::Playing, SessionPanel::StillRunning,
                                     SessionPanel::Confirm, SessionPanel::Outcome}) {
        const std::string phase(rv::app::toString(panel));
        INFO(phase);
        CHECK(contains(rml, "phase == '" + phase + "'"));
    }
    // Every code the outcome panel shows (showsMessage) has a title and a text.
    for (const std::string_view code :
         {lo::kQuickExit, lo::kProcessesRemained, lo::kCrashed, lo::kFailedToStart, lo::kNotConfigured,
          lo::kEmulatorMissing, lo::kCoreMissing, lo::kRomMissing, lo::kLeftRunning}) {
        INFO(code);
        CHECK(contains(rml, "<h1 data-if=\"code == '" + std::string(code) + "'\">"));
        CHECK(contains(rml, "code == '" + std::string(code) + "'"));
    }
    // And every reason of the codes that have reasons.
    for (const std::string_view reason :
         {lo::kReasonStartFailed, lo::kReasonLoaderError, lo::kReasonEarlyExit, lo::kReasonInvalidRequest}) {
        INFO(reason);
        CHECK(contains(rml, "code == 'failed-to-start' && reason == '" + std::string(reason) + "'"));
    }
    for (const std::string_view reason : {lo::kReasonTemplate, lo::kReasonProgramRefused, lo::kReasonInvalidRequest,
                                          lo::kReasonUnknownProfile, lo::kReasonNoLaunch}) {
        INFO(reason);
        CHECK(contains(rml, "code == 'not-configured' && reason == '" + std::string(reason) + "'"));
    }
    // The reasons of a game left running that the panel shows (a return the user chose shows nothing).
    for (const std::string_view reason : {lo::kReasonQuit, lo::kReasonWaitLimit}) {
        INFO(reason);
        CHECK(contains(rml, "code == 'left-running' && reason == '" + std::string(reason) + "'"));
    }
    // A game of the ES-DE catalog that is not ready (I2 step 13): a title, a text per readiness (a fallback for the
    // others, "unresolved" among them), its blocker's words on the "Dettagli" line.
    CHECK(contains(rml, "<h1 data-if=\"code == '" + std::string(lo::kNotReady) + "'\">"));
    for (const std::string_view readiness :
         {"source-unavailable", "rom-missing", "requires-configuration", "emulator-not-found", "core-not-found"}) {
        INFO(readiness);
        CHECK(contains(rml, "code == 'not-ready' && reason == '" + std::string(readiness) + "'"));
    }
    CHECK(contains(rml, "(code == 'not-ready' && detail != '')"));
    // ADR-0006's hint for a quick exit and for processes left in the tree: the profile must wait for the process tree.
    CHECK(contains(rml, "\"wait\": \"tree\""));
    // The runner's and the plan's own words never sit inside an Italian sentence: they show apart.
    CHECK(contains(rml, "Dettagli: {{ detail }}"));
    for (const std::string_view reason : {lo::kReasonStartFailed, lo::kReasonInvalidRequest, lo::kReasonProgramRefused}) {
        INFO(reason);
        CHECK(contains(rml, "reason == '" + std::string(reason) + "' ||"));
    }
    // The panel's Italian strings live here, among them the panel's title while a game runs.
    CHECK(contains(rml, "Gioco in corso"));
    CHECK(contains(rml, "Torna a Retroverse"));
    CHECK(contains(rml, "Torna al gioco"));
    CHECK(contains(rml, "Sono tornato"));
    CHECK(contains(rml, "Continua ad aspettare"));
}
