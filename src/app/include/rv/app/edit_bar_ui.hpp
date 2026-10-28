#pragma once

// GPU-free (part of rv_app_cli, header only): the asset paths, element ids and data model that the editor's bar (rv_app's
// EditBarPanel, I4b step 3; ADR-0015 §4) shares with its RmlUi document (assets/ui/edit_bar.rml), so that GPU-free
// tests check the document against them (tests/unit/app/test_edit_bar_document.cpp). Every sentence is the document's
// (Italian): the code passes titles, counts and keys.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace rv::app {

namespace edit_bar {

inline constexpr std::string_view kDocumentPath = "ui/edit_bar.rml";
inline constexpr std::string_view kStyleSheetPath = "ui/edit_bar.rcss";
/// The data model (EditBarContent's fields, by the same names).
inline constexpr std::string_view kDataModel = "edit_bar";
inline constexpr std::string_view kBarId = "edit-bar";
inline constexpr std::string_view kMenuId = "edit-menu";
/// The menu's buttons, top to bottom (EditMenuButton's order).
inline constexpr std::array<std::string_view, 4> kMenuButtonIds{"edit-save", "edit-undo", "edit-discard",
                                                                "edit-continue"};
/// The modes the bar shows (EditMode's names).
inline constexpr std::array<std::string_view, 6> kModes{"preparing", "browse", "holding", "busy", "menu", "saving"};
/// The rules a refusal names in a sentence of its own (world::edit_rule and the validation's rules a game's move can
/// break; "failed": an edit that could not be made, "save-failed": a save that wrote nothing for an error); another rule
/// reads "La modifica non rispetta le regole della stanza".
inline constexpr std::array<std::string_view, 9> kRules{"game-unknown", "slot-taken", "slot-pinned",
                                                        "other-room",   "same-game",  "slot-unknown",
                                                        "pin-broken",   "failed",     "save-failed"};
/// The devices whose glyphs the bar shows ("pad": the pad's buttons, "keys": the keyboard's).
inline constexpr std::array<std::string_view, 2> kDevices{"pad", "keys"};

} // namespace edit_bar

/// What the editor's bar shows (edit_bar.rml).
struct EditBarContent {
    /// EditMode's name (edit_bar::kModes).
    std::string mode;
    /// The edited room's title (its system's full name, or the shared room's kind: "shared", "arcade" in `roomKind`).
    std::string roomTitle;
    std::string roomKind;
    /// The target under the reticle: none, a game ("game": its title, pinned or not), or an empty slot ("slot": bare,
    /// or "dressed"); whether it shows face-out.
    std::string target;
    std::string targetTitle;
    bool targetPinned = false;
    bool targetFaceOut = false;
    /// The game held (Holding, Busy over a move or a swap): its title; "" none. `targetOwn`: the target is the slot it
    /// was taken from (interact puts it back).
    bool holding = false;
    std::string heldTitle;
    bool targetOwn = false;
    /// While Saving: "save" (Salva) or "discard" (Esci senza salvare: the room shown as stored again).
    std::string ending;
    /// The draft's operations.
    std::uint32_t pending = 0;
    /// The last refusal's rule (edit_bar::kRules or another; "" none) and, for a save refused after a replay, whether it
    /// was the save's ("Il mondo è cambiato nel frattempo"; a save that failed for an error is "save-failed" alone).
    std::string refusal;
    bool saveRefused = false;
    /// The menu's focused button (edit_bar::kMenuButtonIds' order: "save", "undo", "discard", "continue").
    std::string menuFocus;
    /// edit_bar::kDevices: the glyphs of the device used last.
    std::string device = "keys";

    bool operator==(const EditBarContent&) const = default;
};

} // namespace rv::app
