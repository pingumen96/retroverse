// The media viewer's and Informazioni's models (I3 step 14b; rv/ui/media_viewer_model.hpp, game_info_model.hpp) and
// their documents (assets/ui/media_viewer.rml, game_info.rml and their style sheets; their screens' GPU side is the
// smoke tests'): the pages in the viewer's order with the missing kinds skipped, paging without wrap, Back, the
// picture's fit, the scrolling of Informazioni; the documents' ids, data bindings, rv-texture source (RmlUi never loads
// the user's files), a label for every kind, a click event per button, no colour transition and no px sizes. Since I5
// step 8 the videos and manuals pages: their places in the order, Activate pausing the clip and opening the manual,
// their states, and the document's sentence for every reason and outcome.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/ui/game_info_model.hpp>
#include <rv/ui/media_viewer_model.hpp>

namespace mv = rv::ui::media_viewer;
namespace gi = rv::ui::game_info;
using rv::ui::InfoStep;
using rv::ui::MediaViewerModel;
using rv::ui::PictureFit;
using rv::ui::PictureState;
using rv::ui::UiCommand;
using rv::ui::ViewerStep;

namespace {

std::string readAsset(std::string_view relative) {
    const std::filesystem::path assets = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    auto text = rv::core::readTextFile(assets / rv::core::pathFromUtf8(relative));
    REQUIRE(text.has_value());
    return *text;
}

/// Equal within a thousandth of a unit (float rounding of the fit's scale).
bool near(PictureFit fit, float width, float height) {
    return std::abs(fit.width - width) < 1.0e-3f && std::abs(fit.height - height) < 1.0e-3f;
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

std::vector<std::string> kindsOf(const MediaViewerModel& model) {
    std::vector<std::string> kinds;
    for (const rv::ui::MediaPage& page : model.pages()) {
        kinds.push_back(page.kind);
    }
    return kinds;
}

} // namespace

TEST_CASE("media viewer: the pages follow the viewer's order, the missing kinds and the others skipped",
          "[ui][media_viewer]") {
    MediaViewerModel model;
    CHECK_FALSE(model.isOpen());
    const std::vector<std::string> kinds{"fanart",  "videos", "screenshots", "covers",
                                         "miximages", "covers", "3dboxes",    "manuals"};
    model.open("g0000000000000001", "Amber Lantern", "Lumen 16 Home System", kinds);
    CHECK(model.isOpen());
    CHECK(kindsOf(model) ==
          std::vector<std::string>{"covers", "3dboxes", "screenshots", "videos", "fanart", "manuals"});
    CHECK(model.index() == 0);
    REQUIRE(model.current() != nullptr);
    CHECK(model.current()->kind == "covers");
    CHECK(model.current()->state == PictureState::Loading);
    CHECK(model.indexOf("screenshots") == 2);
    CHECK_FALSE(model.indexOf("backcovers").has_value());
    CHECK(model.title() == "Amber Lantern");
    CHECK(model.platform() == "Lumen 16 Home System");

    // Every kind the viewer knows, in its order (ADR-0012 §5).
    std::vector<std::string> all(mv::kKinds.rbegin(), mv::kKinds.rend());
    model.open("g0000000000000002", "", "", all);
    CHECK(kindsOf(model) == std::vector<std::string>(mv::kKinds.begin(), mv::kKinds.end()));
    // I5 step 8 (ADR-0016 §10): the clip after the title screen, the manual last.
    CHECK(kindsOf(model) == std::vector<std::string>{"covers", "backcovers", "physicalmedia", "3dboxes",
                                                     "screenshots", "titlescreens", "videos", "marquees", "fanart",
                                                     "manuals"});
    CHECK(rv::ui::isPictureKind("covers"));
    CHECK(rv::ui::isPictureKind("fanart"));
    CHECK_FALSE(rv::ui::isPictureKind("videos"));
    CHECK_FALSE(rv::ui::isPictureKind("manuals"));
    CHECK_FALSE(rv::ui::isPictureKind("miximages"));

    // A game without images: no page, nothing moves.
    model.open("g0000000000000003", "", "", {});
    CHECK(model.pages().empty());
    CHECK(model.current() == nullptr);
    CHECK(model.apply(UiCommand::Right) == ViewerStep::None);
    CHECK(model.apply(UiCommand::End) == ViewerStep::None);
    CHECK(model.apply(UiCommand::Back) == ViewerStep::Close);
}

TEST_CASE("media viewer: left and right page without wrapping, Home and End jump, Back closes",
          "[ui][media_viewer]") {
    MediaViewerModel model;
    const std::vector<std::string> kinds{"covers", "backcovers", "screenshots"};
    model.open("g0000000000000001", "A", "P", kinds);
    CHECK(model.apply(UiCommand::Left) == ViewerStep::None); // the first page: no wrap
    CHECK(model.apply(UiCommand::Right) == ViewerStep::Moved);
    CHECK(model.index() == 1);
    CHECK(model.apply(UiCommand::Activate) == ViewerStep::Moved);
    CHECK(model.index() == 2);
    CHECK(model.apply(UiCommand::Right) == ViewerStep::None); // the last page: no wrap
    CHECK(model.apply(UiCommand::PageDown) == ViewerStep::None);
    CHECK(model.apply(UiCommand::FilterPrevious) == ViewerStep::Moved);
    CHECK(model.index() == 1);
    CHECK(model.apply(UiCommand::PageUp) == ViewerStep::Moved);
    CHECK(model.index() == 0);
    CHECK(model.apply(UiCommand::End) == ViewerStep::Moved);
    CHECK(model.index() == 2);
    CHECK(model.apply(UiCommand::Home) == ViewerStep::Moved);
    CHECK(model.index() == 0);
    CHECK(model.apply(UiCommand::FilterNext) == ViewerStep::Moved);
    CHECK(model.apply(UiCommand::Up) == ViewerStep::None);
    CHECK(model.apply(UiCommand::Down) == ViewerStep::None);
    CHECK(model.apply(UiCommand::Media) == ViewerStep::None);
    CHECK(model.apply(UiCommand::Back) == ViewerStep::Close);
    CHECK(model.select(1) == ViewerStep::None); // already shown
    CHECK(model.select(7) == ViewerStep::None);
    CHECK(model.select(0) == ViewerStep::Moved);

    // Pictures: shown with their size, failed without; another opening starts over.
    model.setPicture(1, PictureState::Shown, 640, 480);
    model.setPicture(2, PictureState::Failed, 512, 448);
    model.setPicture(9, PictureState::Shown, 1, 1);
    CHECK(model.pages()[1] == rv::ui::MediaPage{"backcovers", PictureState::Shown, 640, 480});
    CHECK(model.pages()[2] == rv::ui::MediaPage{"screenshots", PictureState::Failed, 0, 0});
    model.open("g0000000000000002", "B", "P", kinds);
    CHECK(model.index() == 0);
    CHECK(model.pages()[1].state == PictureState::Loading);

    model.close();
    CHECK_FALSE(model.isOpen());
    CHECK(model.apply(UiCommand::Right) == ViewerStep::None);
    CHECK(model.apply(UiCommand::Back) == ViewerStep::None);
}

TEST_CASE("media viewer: the videos page pauses its clip with Activate, the manuals page opens the manual",
          "[ui][media_viewer]") {
    MediaViewerModel model;
    const std::vector<std::string> kinds{"covers", "videos", "manuals"};
    model.open("g0000000000000001", "Amber Lantern", "P", kinds);
    REQUIRE(kindsOf(model) == kinds);
    // A picture's page: Activate pages on, as before.
    CHECK(model.apply(UiCommand::Activate) == ViewerStep::Moved);
    REQUIRE(model.current()->kind == "videos");
    // The clip before its first frame: Activate does nothing (nothing to pause yet), nor pages on.
    CHECK(model.apply(UiCommand::Activate) == ViewerStep::None);
    CHECK(model.index() == 1);
    model.setVideo(1, PictureState::Shown, 1280, 720);
    CHECK(model.current()->state == PictureState::Shown);
    CHECK(model.current()->width == 1280);
    CHECK(model.apply(UiCommand::Activate) == ViewerStep::TogglePause);
    CHECK(model.current()->paused);
    CHECK(model.apply(UiCommand::Activate) == ViewerStep::TogglePause);
    CHECK_FALSE(model.current()->paused);
    CHECK(model.apply(UiCommand::Activate) == ViewerStep::TogglePause);
    // Leaving the page drops its pause: the clip shown again plays.
    CHECK(model.apply(UiCommand::Right) == ViewerStep::Moved);
    CHECK_FALSE(model.pages()[1].paused);
    CHECK(model.apply(UiCommand::Left) == ViewerStep::Moved);
    // A clip that cannot play: its reason, no pause.
    model.setVideo(1, PictureState::Failed, 640, 480, "unsupported-codec");
    CHECK(model.pages()[1] == rv::ui::MediaPage{"videos", PictureState::Failed, 0, 0, false, "unsupported-codec"});
    CHECK(model.apply(UiCommand::Activate) == ViewerStep::None);
    // setVideo and the manual's setters touch their own kinds only.
    model.setVideo(0, PictureState::Failed, 0, 0, "x");
    model.setManualFile(0, PictureState::Failed, "x.pdf", "file-not-found");
    CHECK(model.pages()[0].state == PictureState::Loading);
    CHECK(model.pages()[0].reason.empty());

    // The manual: nothing until its file is found, then "Apri il manuale".
    CHECK(model.apply(UiCommand::End) == ViewerStep::Moved);
    REQUIRE(model.current()->kind == "manuals");
    model.setManualFile(2, PictureState::Loading, "Amber Lantern (USA).pdf");
    CHECK(model.current()->name == "Amber Lantern (USA).pdf");
    CHECK(model.apply(UiCommand::Activate) == ViewerStep::None);
    model.setManualFile(2, PictureState::Shown, "Amber Lantern (USA).pdf");
    CHECK(model.apply(UiCommand::Activate, 10.0) == ViewerStep::OpenManual);
    CHECK(model.index() == 2); // no page past the last
    model.setManualOutcome(2, rv::ui::ManualState::Refused, "guard");
    CHECK(model.current()->manual == rv::ui::ManualState::Refused);
    CHECK(model.current()->reason == "guard");
    // A refusal, a failure or an unsupported path: asked again at once.
    CHECK(model.apply(UiCommand::Activate, 10.1) == ViewerStep::OpenManual);
    model.setManualOutcome(2, rv::ui::ManualState::Unsupported, "unsupported");
    CHECK(model.current()->manual == rv::ui::ManualState::Unsupported);
    CHECK(model.current()->reason == "unsupported");
    CHECK(model.apply(UiCommand::Activate, 10.2) == ViewerStep::OpenManual);
    model.setManualOutcome(2, rv::ui::ManualState::Opened, "ignored", 10.3);
    CHECK(model.current()->manual == rv::ui::ManualState::Opened);
    CHECK(model.current()->reason.empty());
    model.setManualFile(2, PictureState::Failed, "Amber Lantern (USA).pdf", "file-not-found");
    CHECK(model.apply(UiCommand::Activate) == ViewerStep::None);
    CHECK(rv::ui::toString(rv::ui::ManualState::Ready) == "ready");
    CHECK(rv::ui::toString(rv::ui::ManualState::Opened) == "opened");
    CHECK(rv::ui::toString(rv::ui::ManualState::Refused) == "refused");
    CHECK(rv::ui::toString(rv::ui::ManualState::Failed) == "failed");
    CHECK(rv::ui::toString(rv::ui::ManualState::Opening) == "opening");
    CHECK(rv::ui::toString(rv::ui::ManualState::Unsupported) == "unsupported");
}

TEST_CASE("media viewer: one explorer.exe at a time for the manual, none again right after it opened",
          "[ui][media_viewer]") {
    MediaViewerModel model;
    const std::vector<std::string> kinds{"manuals"};
    model.open("g0000000000000001", "Amber Lantern", "P", kinds);
    model.setManualFile(0, PictureState::Shown, "Amber Lantern (USA).pdf");
    // The first Activate asks for it: the page's manual is opening, and another Activate (a click, a repeat) does
    // nothing until its outcome comes.
    CHECK(model.apply(UiCommand::Activate, 100.0) == ViewerStep::OpenManual);
    CHECK(model.current()->manual == rv::ui::ManualState::Opening);
    CHECK(model.apply(UiCommand::Activate, 100.0) == ViewerStep::None);
    CHECK(model.apply(UiCommand::Activate, 105.0) == ViewerStep::None);
    // Opened at 105: nothing for kManualReopenSeconds, then asked again.
    model.setManualOutcome(0, rv::ui::ManualState::Opened, {}, 105.0);
    CHECK(model.apply(UiCommand::Activate, 105.5) == ViewerStep::None);
    CHECK(model.apply(UiCommand::Activate, 105.0 + rv::ui::media_viewer::kManualReopenSeconds - 0.01) ==
          ViewerStep::None);
    CHECK(model.current()->manual == rv::ui::ManualState::Opened);
    CHECK(model.apply(UiCommand::Activate, 105.0 + rv::ui::media_viewer::kManualReopenSeconds) ==
          ViewerStep::OpenManual);
    CHECK(model.current()->manual == rv::ui::ManualState::Opening);
    // A failed start: asked again at once.
    model.setManualOutcome(0, rv::ui::ManualState::Failed, "process-failed");
    CHECK(model.apply(UiCommand::Activate, 107.5) == ViewerStep::OpenManual);
    // Paging never asks for it.
    CHECK(model.apply(UiCommand::Right, 200.0) == ViewerStep::None);
    CHECK(rv::ui::media_viewer::kManualReopenSeconds == 2.0);
}

TEST_CASE("media viewer: the picture fits the stage without stretching, enlarged at most four times",
          "[ui][media_viewer]") {
    // A 16:9 fan art in a wider stage: as high as the stage.
    CHECK(near(rv::ui::fitPicture(1200.0f, 500.0f, 1280, 720), 500.0f * 1280.0f / 720.0f, 500.0f));
    // A portrait box in a landscape stage: as high as the stage; a wide marquee: as wide.
    CHECK(near(rv::ui::fitPicture(1000.0f, 560.0f, 560, 700), 448.0f, 560.0f));
    CHECK(near(rv::ui::fitPicture(1000.0f, 560.0f, 800, 300), 1000.0f, 375.0f));
    // A screenshot enlarged to the stage; a 16 px picture at most four times.
    CHECK(near(rv::ui::fitPicture(1000.0f, 560.0f, 512, 448), 640.0f, 560.0f));
    CHECK(near(rv::ui::fitPicture(1000.0f, 560.0f, 16, 16), 64.0f, 64.0f));
    CHECK(rv::ui::fitPicture(0.0f, 560.0f, 16, 16) == PictureFit{});
    CHECK(rv::ui::fitPicture(1000.0f, 560.0f, 0, 16) == PictureFit{});
    CHECK(rv::ui::toString(PictureState::Loading) == "loading");
    CHECK(rv::ui::toString(PictureState::Shown) == "shown");
    CHECK(rv::ui::toString(PictureState::Failed) == "failed");
}

TEST_CASE("Informazioni: the commands scroll its body, Back closes it, Y opens the viewer", "[ui][media_viewer]") {
    CHECK(rv::ui::infoStepOf(UiCommand::Up) == InfoStep::ScrollUp);
    CHECK(rv::ui::infoStepOf(UiCommand::Down) == InfoStep::ScrollDown);
    CHECK(rv::ui::infoStepOf(UiCommand::PageUp) == InfoStep::PageUp);
    CHECK(rv::ui::infoStepOf(UiCommand::FilterPrevious) == InfoStep::PageUp);
    CHECK(rv::ui::infoStepOf(UiCommand::PageDown) == InfoStep::PageDown);
    CHECK(rv::ui::infoStepOf(UiCommand::FilterNext) == InfoStep::PageDown);
    CHECK(rv::ui::infoStepOf(UiCommand::Home) == InfoStep::Top);
    CHECK(rv::ui::infoStepOf(UiCommand::End) == InfoStep::Bottom);
    CHECK(rv::ui::infoStepOf(UiCommand::Back) == InfoStep::Close);
    CHECK(rv::ui::infoStepOf(UiCommand::Media) == InfoStep::Media);
    CHECK(rv::ui::infoStepOf(UiCommand::Left) == InfoStep::None);
    CHECK(rv::ui::infoStepOf(UiCommand::Activate) == InfoStep::None);
    CHECK(rv::ui::toString(InfoStep::PageDown) == "page-down");

    // A body 300 high with 1,000 of content: lines of 40, pages of 260, clamped to [0, 700].
    CHECK(rv::ui::scrolledTop(0.0f, InfoStep::ScrollDown, 300.0f, 1000.0f, 40.0f) == 40.0f);
    CHECK(rv::ui::scrolledTop(0.0f, InfoStep::ScrollUp, 300.0f, 1000.0f, 40.0f) == 0.0f);
    CHECK(rv::ui::scrolledTop(40.0f, InfoStep::PageDown, 300.0f, 1000.0f, 40.0f) == 300.0f);
    CHECK(rv::ui::scrolledTop(600.0f, InfoStep::PageDown, 300.0f, 1000.0f, 40.0f) == 700.0f);
    CHECK(rv::ui::scrolledTop(300.0f, InfoStep::PageUp, 300.0f, 1000.0f, 40.0f) == 40.0f);
    CHECK(rv::ui::scrolledTop(300.0f, InfoStep::Bottom, 300.0f, 1000.0f, 40.0f) == 700.0f);
    CHECK(rv::ui::scrolledTop(300.0f, InfoStep::Top, 300.0f, 1000.0f, 40.0f) == 0.0f);
    // Content that fits: nothing to scroll.
    CHECK(rv::ui::scrolledTop(0.0f, InfoStep::ScrollDown, 300.0f, 200.0f, 40.0f) == 0.0f);
    CHECK(rv::ui::scrolledTop(0.0f, InfoStep::Close, 300.0f, 1000.0f, 40.0f) == 0.0f);
}

TEST_CASE("media viewer's document: ids, data model, the rv-texture picture, a label per kind, the buttons' events",
          "[ui][media_viewer]") {
    const std::string rml = readAsset(mv::kDocumentPath);
    CHECK(contains(rml, "data-model=\"" + std::string(mv::kDataModel) + "\""));
    for (const std::string_view id : {mv::kViewerId, mv::kTitleId, mv::kStageId, mv::kPictureId, mv::kPreviousId,
                                      mv::kNextId, mv::kCloseId, mv::kVideoId, mv::kPauseButtonId,
                                      mv::kManualButtonId}) {
        INFO(id);
        CHECK(contains(rml, "id=\"" + std::string(id) + "\""));
    }
    // The picture is the application's texture: RmlUi never loads the user's files (no other image in the document).
    CHECK(contains(rml, "src=\"" + std::string(mv::kPictureSource) + "\""));
    CHECK(std::string(mv::kPictureSource) == "rv-texture:" + std::string(mv::kPictureTexture));
    std::size_t images = 0;
    for (std::size_t at = rml.find("<img"); at != std::string::npos; at = rml.find("<img", at + 1)) {
        ++images;
    }
    // I5 step 8: the clip is the application's texture too.
    CHECK(contains(rml, "src=\"" + std::string(mv::kVideoSource) + "\""));
    CHECK(std::string(mv::kVideoSource) == "rv-texture:" + std::string(mv::kVideoTexture));
    CHECK(images == 2);
    CHECK(contains(rml, "data-style-width=\"viewer.width\""));
    CHECK(contains(rml, "data-style-height=\"viewer.height\""));
    // Every kind has its label; the loading, failed and empty states their sentences.
    for (const std::string_view kind : mv::kKinds) {
        INFO(kind);
        CHECK(contains(rml, "viewer.kind == '" + std::string(kind) + "'"));
    }
    for (const std::string_view state : {"viewer.state == 'shown'", "viewer.state == 'loading'",
                                         "viewer.state == 'failed'", "viewer.count == 0"}) {
        INFO(state);
        CHECK(contains(rml, state));
    }
    for (const std::string_view click :
         {"page('previous')", "page('next')", "page('close')", "page('pause')", "page('manual')"}) {
        INFO(click);
        CHECK(contains(rml, "data-event-click=\"" + std::string(click) + "\""));
    }
    // I5 step 8: a sentence for every reason a clip cannot play, the generic one, the reason shown; the manual's file,
    // its button and every outcome; the pause.
    for (const std::string_view reason : mv::kVideoReasons) {
        INFO(reason);
        CHECK(contains(rml, "viewer.reason == '" + std::string(reason) + "'"));
    }
    for (const std::string_view part :
         {"!viewer.knownReason", "{{ viewer.reason }}", "{{ viewer.name }}", "viewer.paused", "Apri il manuale",
          "viewer.manual == 'opened'", "viewer.manual == 'refused'", "viewer.manual == 'failed'",
          "viewer.manual == 'ready'"}) {
        INFO(part);
        CHECK(contains(rml, part));
    }
    for (const auto outcome : {rv::ui::ManualState::Opened, rv::ui::ManualState::Refused, rv::ui::ManualState::Failed,
                               rv::ui::ManualState::Unsupported}) {
        CHECK(contains(rml, "viewer.manual == '" + std::string(rv::ui::toString(outcome)) + "'"));
    }
    const std::string rcss = readAsset(mv::kStyleSheetPath);
    CHECK_FALSE(contains(rcss, "transition"));
    CHECK_FALSE(contains(rcss, "px;"));
    CHECK_FALSE(contains(rcss, "px "));
    // The buttons never take RmlUi's focus: the application keeps the input.
    CHECK(contains(rcss, "focus: none"));
}

TEST_CASE("Informazioni's document: ids, data model, every field with its n.d., the versions, the buttons' events",
          "[ui][media_viewer]") {
    const std::string rml = readAsset(gi::kDocumentPath);
    CHECK(contains(rml, "data-model=\"" + std::string(gi::kDataModel) + "\""));
    for (const std::string_view id :
         {gi::kInfoId, gi::kTitleId, gi::kBodyId, gi::kDescriptionId, gi::kVersionsId, gi::kCloseId, gi::kMediaId}) {
        INFO(id);
        CHECK(contains(rml, "id=\"" + std::string(id) + "\""));
    }
    for (const std::string_view field : {"developer", "publisher", "genre", "players", "platform", "year"}) {
        INFO(field);
        CHECK(contains(rml, "info." + std::string(field) + " == ''\">n.d."));
    }
    CHECK(contains(rml, "{{ info.description }}"));
    CHECK(contains(rml, "data-for=\"version : versions\""));
    CHECK(contains(rml, "{{ version.title }}"));
    CHECK(contains(rml, "{{ version.region }}"));
    CHECK(contains(rml, "info.versionCount == 0"));
    CHECK(contains(rml, "info.more"));
    CHECK_FALSE(contains(rml, "<img"));
    for (const std::string_view click : {"act('close')", "act('media')"}) {
        INFO(click);
        CHECK(contains(rml, "data-event-click=\"" + std::string(click) + "\""));
    }
    const std::string rcss = readAsset(gi::kStyleSheetPath);
    CHECK_FALSE(contains(rcss, "transition"));
    CHECK_FALSE(contains(rcss, "px;"));
    CHECK_FALSE(contains(rcss, "px "));
    CHECK(contains(rcss, "white-space: pre-line"));
    CHECK(contains(rcss, "overflow-y: auto"));
}
