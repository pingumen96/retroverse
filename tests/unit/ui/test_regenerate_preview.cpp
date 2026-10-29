// The impact preview's model and its document (I4b step 6; ADR-0015 §5, §6): rv::ui::RegeneratePreviewModel's states,
// buttons, focus and commands (the three choices, the second confirmation of the drop, the undo, the failures, the
// focus a content arrives with and the input frame its Activate waits, the device of the hints), and
// assets/ui/regenerate_preview.rml against the ids, bindings and keys its screen uses.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/ui/directory_model.hpp>
#include <rv/ui/regenerate_preview_model.hpp>

namespace rp = rv::ui::regenerate_preview;
using rv::ui::RegenerateButton;
using rv::ui::RegenerateCommandKind;
using rv::ui::RegenerateKind;
using rv::ui::RegeneratePreviewContent;
using rv::ui::RegeneratePreviewModel;
using rv::ui::RegenerateState;
using rv::ui::UiCommand;

namespace {

RegeneratePreviewContent impact(bool changed, std::uint32_t overrides) {
    RegeneratePreviewContent content;
    content.roomsRegenerated = 1;
    content.gamesMoved = 33;
    content.pinsKept = overrides;
    content.overrides = overrides;
    content.changed = changed;
    return content;
}

RegeneratePreviewModel opened(RegenerateKind kind = RegenerateKind::Room) {
    RegeneratePreviewModel model;
    model.open(kind, rv::ui::RegenerateScopeTitle{"console", "Super Nintendo", ""});
    return model;
}

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

TEST_CASE("ui/regenerate_preview: names and element ids", "[ui][regenerate]") {
    for (const RegenerateButton button :
         {RegenerateButton::Regenerate, RegenerateButton::Drop, RegenerateButton::ConfirmDrop, RegenerateButton::Undo,
          RegenerateButton::Back, RegenerateButton::Cancel}) {
        CHECK(rv::ui::regenerateButtonFromName(rv::ui::toString(button)) == button);
        CHECK(std::string(rv::ui::elementIdOf(button)).starts_with("rp-"));
    }
    CHECK_FALSE(rv::ui::regenerateButtonFromName("other").has_value());
    CHECK(rv::ui::toString(RegenerateKind::Floor) == "floor");
    CHECK(rv::ui::toString(RegenerateState::Confirm) == "confirm");
    CHECK(rv::ui::toString(RegenerateCommandKind::RegenerateDrop) == "regenerate-drop");
}

TEST_CASE("ui/regenerate_preview: computing, Annulla alone; Back closes it with a cancel", "[ui][regenerate]") {
    RegeneratePreviewModel model = opened();
    CHECK(model.isOpen());
    CHECK(model.state() == RegenerateState::Computing);
    CHECK(model.buttons() == std::vector<RegenerateButton>{RegenerateButton::Cancel});
    CHECK(model.focusedElement() == rp::kCancelId);
    CHECK(model.scope().roomTitle == "Super Nintendo");
    CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    CHECK(model.apply(UiCommand::Back) == RegenerateCommandKind::Cancel);
    CHECK_FALSE(model.isOpen());
    CHECK(model.buttons().empty());
    // A closed model takes nothing.
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
}

TEST_CASE("ui/regenerate_preview: the three choices; the drop asks a second confirmation, Indietro first",
          "[ui][regenerate]") {
    RegeneratePreviewModel model = opened();
    const std::uint64_t computing = model.revision();
    model.setContent(impact(true, 2));
    model.endInputFrame();
    CHECK(model.revision() > computing);
    CHECK(model.state() == RegenerateState::Ready);
    CHECK(model.content().gamesMoved == 33);
    CHECK(model.buttons() == std::vector<RegenerateButton>{RegenerateButton::Regenerate, RegenerateButton::Drop,
                                                           RegenerateButton::Cancel});
    CHECK(model.focused() == RegenerateButton::Regenerate);
    CHECK(model.focusedElement() == rp::kRegenerateId);
    // Up stops at the first button, Down at the last.
    CHECK_FALSE(model.apply(UiCommand::Up).has_value());
    CHECK(model.focused() == RegenerateButton::Regenerate);
    CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    CHECK(model.focused() == RegenerateButton::Drop);
    // The drop: no command, the second confirmation with "Indietro" focused.
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.state() == RegenerateState::Confirm);
    // "Indietro" above "Scarta e rigenera" (the drop's row, counted from the panel's bottom, is Indietro's).
    CHECK(model.buttons() == std::vector<RegenerateButton>{RegenerateButton::Back, RegenerateButton::ConfirmDrop});
    CHECK(model.focused() == RegenerateButton::Back);
    CHECK(model.focusedElement() == rp::kBackId);
    // Pressed twice, the button drops nothing: it goes back to the preview, the drop focused.
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.state() == RegenerateState::Ready);
    CHECK(model.focused() == RegenerateButton::Drop);
    // Back from the confirmation is the preview again, never a cancel.
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.state() == RegenerateState::Confirm);
    CHECK_FALSE(model.apply(UiCommand::Back).has_value());
    CHECK(model.isOpen());
    CHECK(model.state() == RegenerateState::Ready);
    // The drop confirmed: committing, no button, Back ignored.
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK_FALSE(model.apply(UiCommand::Right).has_value());
    CHECK(model.focused() == RegenerateButton::ConfirmDrop);
    CHECK(model.apply(UiCommand::Activate) == RegenerateCommandKind::RegenerateDrop);
    CHECK(model.state() == RegenerateState::Committing);
    CHECK(model.buttons().empty());
    CHECK(model.focusedElement().empty());
    CHECK_FALSE(model.apply(UiCommand::Back).has_value());
    CHECK(model.isOpen());
    // The world closes it once shown.
    model.close();
    CHECK_FALSE(model.isOpen());
}

TEST_CASE("ui/regenerate_preview: Rigenera keeps the overrides; nothing to change offers only the drop or Chiudi",
          "[ui][regenerate]") {
    RegeneratePreviewModel keep = opened();
    keep.setContent(impact(true, 0));
    keep.endInputFrame();
    CHECK(keep.buttons() == std::vector<RegenerateButton>{RegenerateButton::Regenerate, RegenerateButton::Cancel});
    CHECK(keep.apply(UiCommand::Activate) == RegenerateCommandKind::Regenerate);
    CHECK(keep.state() == RegenerateState::Committing);

    RegeneratePreviewModel unchanged = opened();
    unchanged.setContent(impact(false, 0));
    unchanged.endInputFrame();
    CHECK(unchanged.buttons() == std::vector<RegenerateButton>{RegenerateButton::Cancel});
    CHECK(unchanged.focused() == RegenerateButton::Cancel);
    CHECK(unchanged.apply(UiCommand::Activate) == RegenerateCommandKind::Cancel);
    CHECK_FALSE(unchanged.isOpen());

    RegeneratePreviewModel pinsOnly = opened();
    pinsOnly.setContent(impact(false, 3));
    CHECK(pinsOnly.buttons() == std::vector<RegenerateButton>{RegenerateButton::Drop, RegenerateButton::Cancel});
    // Only the drop: Annulla focused (I4b step 6's review: a focus never rests first on what cannot be undone).
    CHECK(pinsOnly.focused() == RegenerateButton::Cancel);
    // A click on a button that does not show does nothing.
    CHECK_FALSE(pinsOnly.click(RegenerateButton::Regenerate).has_value());
    CHECK(pinsOnly.state() == RegenerateState::Ready);
    CHECK_FALSE(pinsOnly.click(RegenerateButton::Drop).has_value());
    CHECK(pinsOnly.state() == RegenerateState::Confirm);
    CHECK(pinsOnly.click(RegenerateButton::ConfirmDrop) == RegenerateCommandKind::RegenerateDrop);
}

TEST_CASE("ui/regenerate_preview: an undo names its action; a failure shows Chiudi; a late answer is ignored",
          "[ui][regenerate]") {
    RegeneratePreviewModel undo = opened(RegenerateKind::Undo);
    RegeneratePreviewContent content;
    content.undoAction = "regenerate";
    content.undoScope = "room:snes";
    content.undoWhen = "29/10/2026 10:15";
    undo.setContent(content);
    undo.endInputFrame();
    CHECK(undo.kind() == RegenerateKind::Undo);
    CHECK(undo.buttons() == std::vector<RegenerateButton>{RegenerateButton::Undo, RegenerateButton::Cancel});
    // "Indietro" focused: no redo brings an undone action back.
    CHECK(undo.focused() == RegenerateButton::Cancel);
    CHECK_FALSE(undo.apply(UiCommand::Up).has_value());
    CHECK(undo.focused() == RegenerateButton::Undo);
    CHECK(undo.apply(UiCommand::Activate) == RegenerateCommandKind::Undo);
    CHECK(undo.state() == RegenerateState::Committing);
    // The commit failed: the panel says why, Chiudi closes it.
    undo.setFailed("changed");
    CHECK(undo.state() == RegenerateState::Failed);
    CHECK(undo.failure() == "changed");
    CHECK(undo.buttons() == std::vector<RegenerateButton>{RegenerateButton::Cancel});
    // A preview's answer once it is no longer computing is ignored.
    undo.setContent(impact(true, 1));
    CHECK(undo.state() == RegenerateState::Failed);
    CHECK(undo.click(RegenerateButton::Cancel) == RegenerateCommandKind::Cancel);
    CHECK_FALSE(undo.isOpen());
    // Nothing happens to a closed model.
    undo.setFailed("busy");
    CHECK_FALSE(undo.isOpen());
}

TEST_CASE("ui/regenerate_preview: a press meant for what showed before never lands on a choice; the hints' device",
          "[ui][regenerate]") {
    RegeneratePreviewModel model = opened();
    model.setContent(impact(true, 1));
    // The first input frame after the content: Activate (held, or doubled from the row that opened it) does nothing.
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.state() == RegenerateState::Ready);
    model.endInputFrame();
    CHECK(model.apply(UiCommand::Activate) == RegenerateCommandKind::Regenerate);
    // Only the content's arrival holds it: the second confirmation's Activate counts at once (Indietro focused).
    RegeneratePreviewModel drop = opened();
    drop.setContent(impact(false, 1));
    drop.endInputFrame();
    CHECK_FALSE(drop.apply(UiCommand::Up).has_value());
    CHECK(drop.focused() == RegenerateButton::Drop);
    CHECK_FALSE(drop.apply(UiCommand::Activate).has_value());
    CHECK(drop.state() == RegenerateState::Confirm);
    CHECK_FALSE(drop.apply(UiCommand::Activate).has_value());
    CHECK(drop.state() == RegenerateState::Ready);
    // The device used last: the hints' glyphs.
    CHECK_FALSE(model.padDevice());
    const std::uint64_t before = model.revision();
    model.setDevice(true);
    CHECK(model.padDevice());
    CHECK(model.revision() > before);
    model.setDevice(true);
    CHECK(model.revision() == before + 1);
}

TEST_CASE("ui/regenerate_preview: the document has the ids, bindings, events and keys its screen uses",
          "[ui][regenerate][documents]") {
    const std::string rml = readAsset(rp::kDocumentPath);
    CHECK(contains(rml, "data-model=\"" + std::string(rp::kDataModel) + "\""));
    const std::string sheet = rv::core::pathToUtf8(rv::core::pathFromUtf8(rp::kStyleSheetPath).filename());
    CHECK(contains(rml, "href=\"" + sheet + "\""));
    CHECK(contains(rml, "id=\"" + std::string(rp::kPanelId) + "\""));
    for (const RegenerateButton button :
         {RegenerateButton::Regenerate, RegenerateButton::Drop, RegenerateButton::ConfirmDrop, RegenerateButton::Undo,
          RegenerateButton::Back, RegenerateButton::Cancel}) {
        INFO(rv::ui::toString(button));
        // One element per id (the focus finds it by its id), its click naming the button.
        const std::string id = "id=\"" + std::string(rv::ui::elementIdOf(button)) + "\"";
        CHECK(contains(rml, id));
        CHECK(rml.find(id) == rml.rfind(id));
        CHECK(contains(rml, "data-event-click=\"" + std::string(rp::kButtonEvent) + "('" +
                                std::string(rv::ui::toString(button)) + "')\""));
    }
    for (const std::string_view kind : rp::kKinds) {
        INFO(kind);
        CHECK(contains(rml, "kind == '" + std::string(kind) + "'"));
    }
    for (const std::string_view state : rp::kStates) {
        INFO(state);
        CHECK(contains(rml, "state == '" + std::string(state) + "'"));
    }
    for (const std::string_view failure : rp::kFailures) {
        INFO(failure);
        CHECK(contains(rml, "failure == '" + std::string(failure) + "'"));
    }
    for (const std::string_view reason : rp::kConflictReasons) {
        INFO(reason);
        CHECK(contains(rml, "conflict.reason == '" + std::string(reason) + "'"));
    }
    for (const std::string_view subject : rp::kConflictSubjects) {
        INFO(subject);
        CHECK(contains(rml, "conflict.subject == '" + std::string(subject) + "'"));
    }
    for (const std::string_view device : rp::kDevices) {
        INFO(device);
        CHECK(contains(rml, "device == '" + std::string(device) + "'"));
    }
    // The column: a body that scrolls, the choices and the hints after it (I4b step 6's review).
    CHECK(contains(rml, "id=\"rp-body\""));
    CHECK(contains(rml, "id=\"rp-hints\""));
    CHECK(rml.find("id=\"rp-body\"") < rml.find("id=\"rp-actions\""));
    CHECK(rml.find("id=\"rp-actions\"") < rml.find("id=\"rp-hints\""));
    for (const std::string_view action : rp::kUndoActions) {
        INFO(action);
        CHECK(contains(rml, "undoAction == '" + std::string(action) + "'"));
    }
    // The floors by the directory's names.
    for (const std::string_view floor : rv::ui::directory::kFloorIds) {
        INFO(floor);
        CHECK(contains(rml, "floor == '" + std::string(floor) + "'"));
        CHECK(contains(rml, "undoFloor == '" + std::string(floor) + "'"));
    }
    for (const std::string_view binding :
         {"{{ roomsRegenerated }}", "{{ gamesMoved }}", "{{ faceOutEntering }}", "{{ faceOutLeaving }}",
          "{{ newlyPlaced }}", "{{ fixturesMoved }}", "{{ pinsKept }}", "{{ conflictCount }}", "{{ overrides }}",
          "{{ conflictsMore }}", "conflict : conflicts", "{{ conflict.title }}", "{{ undoWhen }}", "showRegenerate",
          "showDrop", "showConfirmDrop", "showUndo", "showBack", "showCancel"}) {
        INFO(binding);
        CHECK(contains(rml, binding));
    }
    // ADR-0015 §5's words.
    for (const std::string_view word :
         {">Rigenera<", ">Rigenera e scarta le personalizzazioni<", ">Annulla<", ">Scarta e rigenera<", ">Indietro<",
          ">Annulla la modifica<", ">Chiudi<", "Rigenera stanza", "Rigenera piano", "Rigenera mondo",
          "Annulla ultima modifica"}) {
        INFO(word);
        CHECK(contains(rml, word));
    }
    const std::string rcss = readAsset(rp::kStyleSheetPath);
    CHECK(contains(rcss, ".action:focus {"));
    CHECK(contains(rcss, "#rp-body {\n\tflex: 1 1 auto;"));
    CHECK(contains(rcss, "overflow-y: auto;"));
    CHECK(contains(rcss, "#rp-actions {\n\tflex: 0 0 auto;"));
    CHECK(contains(rcss, "flex-direction: column;"));
    CHECK(contains(rcss, "@media (theme: compact)"));
    CHECK_FALSE(contains(rcss, "transition"));
    CHECK_FALSE(contains(rcss, "px;"));
}
