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
/// toString(CatalogState)), "room" (the start room's id as --start-room gives it, "room:snes", "area:arcade" or
/// "area:shared:<floor>", for kNoGames; empty without --start-room), "roomKind" and "roomTitle" (kNeedsConfirmation's
/// and kRoomsAdded's room: "console", "shared" or "arcade", and a console room's system's name), "roomNumber" and
/// "moreRooms" (kRoomsAdded: the new room's number in its chain, and how many other rooms the update added),
/// "groundFloor" (kRoomsAdded: the new room stands on the ground floor, reached from its atrium, not a floor's hall).
inline constexpr std::string_view kDataModel = "world_notice";
inline constexpr std::string_view kPanelId = "world-notice";

/// No notice.
inline constexpr std::string_view kNone = "";
/// The room is being made: the empty shell shows.
inline constexpr std::string_view kGenerating = "generating";
/// The test room instead of the generated one: the ES-DE catalog did not open ("state" says why), or no system of it
/// gives the building a room (and no plan is stored: I4 step 5a; "room" names the start room when one is asked).
inline constexpr std::string_view kNoCatalog = "no-catalog";
inline constexpr std::string_view kNoGames = "no-games";
/// The media root is out of reach: no plan yet (the shell), or the stored plan shown as it was.
inline constexpr std::string_view kMediaUnknown = "media-unknown";
inline constexpr std::string_view kMediaUnknownKept = "media-unknown-kept";
/// The stored plan shown as it was: another seed, preset, template or generator major version (ADR-0010 §10); or the
/// room shown kept as it is by an update that saved the building (ADR-0013 §5: its template version, prefab levels or
/// generator major changed), its new games unplaced as room-kept.
inline constexpr std::string_view kRegeneration = "regeneration";
/// The stored plan cannot be used (edited by hand, written by a newer Retroverse): the shell.
inline constexpr std::string_view kRefused = "refused";
/// The room cannot be made (a plan that breaks a hard rule, an error): the shell.
inline constexpr std::string_view kFailed = "failed";
/// I4 step 7b: the quick catalog's "Mostra nella stanza" could not take the walker to its game's room (refused for now:
/// a floor being applied, a launch; or its ride's floor could not be built): the walker stays where it was. Shown over
/// the floor for kShownSeconds.
inline constexpr std::string_view kRoomUnreachable = "room-unreachable";
/// I6 step 4 (ADR-0014 §2): the update that saved the building migrated rooms of cases and boxes once to their
/// families' successors (standing on their side, the spine out; since I6 step 6 the arcade room's cases to its
/// cabinets' screens): shown over the floor for kShownSeconds, once (the next update migrates nothing).
inline constexpr std::string_view kRoomsMigrated = "rooms-migrated";
/// I4b step 3 (ADR-0015 §2, §4): the editor cannot edit the room now (overrides.json written by a newer Retroverse, the
/// world's data or lock out of reach, the room not ready): nothing is edited. Shown for kShownSeconds.
inline constexpr std::string_view kEditUnavailable = "edit-unavailable";

/// I4b step 6 (ADR-0015 §3, §5): the update left something undone because it would have moved what the user pinned
/// (its needsConfirmation: a room that cannot grow past a pinned fixture, a pinned room not migrated): the notice
/// offers "Rigenera stanza" of that room, whose preview confirm (Enter, the pad's Start) opens while it shows in free
/// roam.
inline constexpr std::string_view kNeedsConfirmation = "needs-confirmation";

/// I4b step 8 (ADR-0015 §7): the update that saved the building added an expansion room to a chain whose rooms stood
/// at their limit (its titles past their capacity, or capacity entries of an earlier generator migrated): the notice
/// names the first new room ("roomKind", "roomTitle", "roomNumber") and counts the others ("moreRooms"), shown over
/// the floor for kShownSeconds, once.
inline constexpr std::string_view kRoomsAdded = "rooms-added";

/// Every key with a sentence of its own.
inline constexpr std::array<std::string_view, 13> kNotices{
    kGenerating,    kNoCatalog,       kNoGames,           kMediaUnknown, kMediaUnknownKept,
    kRegeneration,  kRefused,         kFailed,            kRoomUnreachable,
    kRoomsMigrated, kEditUnavailable, kNeedsConfirmation, kRoomsAdded};
/// The catalog states kNoCatalog has a sentence for (CatalogState but Ready and Opening).
inline constexpr std::array<std::string_view, 5> kCatalogStates{"no-library", "no-database", "data-directory",
                                                                "library-mismatch", "unavailable"};

/// How long a notice shown over a usable room stays (seconds of the run's time): the test room's, the kept plan's. The
/// notices of the shell stay while it shows.
inline constexpr double kShownSeconds = 12.0;

} // namespace rv::app::world_notice
