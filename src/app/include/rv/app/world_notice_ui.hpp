#pragma once

// GPU-free (part of rv_app_cli, header only): the asset paths, element ids, data model and notice keys that the world's
// notice (rv_app's WorldNoticePanel, I3 step 10b) shares with its RmlUi document (assets/ui/world_notice.rml), so that
// GPU-free tests check the document against them (tests/unit/app/test_world_notice_document.cpp). The sentences are the
// document's: the code only names a key.

#include <array>
#include <string_view>

namespace rv::app::world_notice {

inline constexpr std::string_view kDocumentPath = "ui/world_notice.rml";
inline constexpr std::string_view kStyleSheetPath = "ui/world_notice.rcss";
/// The data model: "notice" (one of the keys below), "state" (the catalog's state for kNoCatalog: catalog_worker.hpp's
/// toString(CatalogState)), "system" (the room's system, ES-DE's name, for kNoGames).
inline constexpr std::string_view kDataModel = "world_notice";
inline constexpr std::string_view kPanelId = "world-notice";

/// No notice.
inline constexpr std::string_view kNone = "";
/// The room is being made: the empty shell shows.
inline constexpr std::string_view kGenerating = "generating";
/// The test room instead of the generated one: the ES-DE catalog did not open ("state" says why), or it has no games
/// of the room's system.
inline constexpr std::string_view kNoCatalog = "no-catalog";
inline constexpr std::string_view kNoGames = "no-games";
/// The media root is out of reach: no plan yet (the shell), or the stored plan shown as it was.
inline constexpr std::string_view kMediaUnknown = "media-unknown";
inline constexpr std::string_view kMediaUnknownKept = "media-unknown-kept";
/// The stored plan shown as it was: another seed, preset, template or generator major version (ADR-0010 §10).
inline constexpr std::string_view kRegeneration = "regeneration";
/// The stored plan cannot be used (edited by hand, written by a newer Retroverse): the shell.
inline constexpr std::string_view kRefused = "refused";
/// The room cannot be made (a plan that breaks a hard rule, an error): the shell.
inline constexpr std::string_view kFailed = "failed";

/// Every key with a sentence of its own.
inline constexpr std::array<std::string_view, 8> kNotices{kGenerating,       kNoCatalog,    kNoGames, kMediaUnknown,
                                                          kMediaUnknownKept, kRegeneration, kRefused, kFailed};
/// The catalog states kNoCatalog has a sentence for (CatalogState but Ready and Opening).
inline constexpr std::array<std::string_view, 5> kCatalogStates{"no-library", "no-database", "data-directory",
                                                                "library-mismatch", "unavailable"};

/// How long a notice shown over a usable room stays (seconds of the run's time): the test room's, the kept plan's. The
/// notices of the shell stay while it shows.
inline constexpr double kShownSeconds = 12.0;

} // namespace rv::app::world_notice
