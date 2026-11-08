#pragma once

// GPU-free (part of rv_app_cli, header only): the asset paths, element ids and data model that the game-session panel
// (rv_app's GameSessionPanel) shares with its RmlUi document, so that GPU-free tests check the document against them
// (tests/unit/app/test_game_session_document.cpp).

#include <string_view>

namespace rv::app::game_session_ui {

/// The panel over the quick catalog while a game runs and after it: "Gioco in corso", the questions of
/// UserConfirmReturn and StillRunning, the outcome's message (LaunchFlow, SessionPanel).
inline constexpr std::string_view kDocumentPath = "ui/game_session.rml";
inline constexpr std::string_view kStyleSheetPath = "ui/game_session.rcss";
/// The data model: "phase" (toString(SessionPanel)), "title" (the game's), "code", "reason" and "detail" (the
/// LaunchMessage), "timeout" (the window timeout in seconds, for StillRunning's text), "exclusive" (the game runs in
/// exclusive fullscreen, I11 step 13), "stopped" (Playing asked in VR after the user stopped watching, I11 step 14),
/// "audio" (the game plays on Windows' default audio output, not the headset's, I11 step 14); the events "return_home",
/// "resume" and "dismiss".
inline constexpr std::string_view kExclusiveNoticeId = "session-exclusive";
inline constexpr std::string_view kAudioNoticeId = "session-audio";
inline constexpr std::string_view kDataModel = "session";
inline constexpr std::string_view kPanelId = "session-panel";
/// The buttons: return to Retroverse ("Torna a Retroverse", "Sono tornato" for a confirmation), back to the game ("Torna
/// al gioco", "Continua ad aspettare"), and the outcome's OK. Each region of buttons is a scroll container, which
/// confines RmlUi's spatial navigation.
inline constexpr std::string_view kReturnId = "session-return";
inline constexpr std::string_view kResumeId = "session-resume";
inline constexpr std::string_view kOkId = "session-ok";

} // namespace rv::app::game_session_ui
