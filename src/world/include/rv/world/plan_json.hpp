#pragma once

// The WorldPlan's canonical JSON (ADR-0010 §8, ADR-0013 §5): the file world-plan.json holds exactly writePlan()'s
// text, written with core::writeCanonicalJson (keys by code point, integers only, one element per line in the
// top-level arrays, a final line feed), so that one plan is one text in Debug and Release and its SHA-256 is the plan
// digest. The reader takes back only such a text: a plan edited by hand or written by another writer is refused, not
// normalized, and so is a plan of a newer schema. Schema 2 is the only one written; schema 1 (I3's one room) is read
// as it is, its content with schemaVersion 1, for world::lift() (lift.hpp) to make a building of it.
//
// The document of schema 2 (every key required unless noted):
//   {"catalogSortKeyVersion": 1, "generatorVersion": "1.3", "inputDigest": "<64 hex>", "library": "esde",
//    "preset": "retro_museum_cozy", "randomAlgorithm": "splitmix64/1", "schemaVersion": 2, "seed": <0..2^64-1>,
//    "titleKeyRule": "title-key/1",
//    "floors": [{"id": "ground", "order": 0}, {"id": "gen4", "order": 3}],
//    "rooms": [{"id": "room:snes", "kind": "console", "floor": "gen4", "sections": ["snes"],
//               "placement": {"xMm": 7650, "zMm": -6650, "yawHalfTurns": 0}, "generatorVersion": "1.3",
//               "template": "console_room_9x10", "templateVersion": 2, "minXMm": -4500, "maxXMm": 4500,
//               "minZMm": -5000, "maxZMm": 5000, "heightMm": 3200, "growthSteps": 0, "seed": <u64>,
//               "mediaFamily": "cartridge_home_16bit", "themePack": "classic_16bit_warm",
//               "expands": "room:snes" (an expansion only)}],
//    "fixtures": [{"id": "room:snes/fx:w01", "role": "side-module",
//                  "prefab": {"kind": "shelf", "manifest": "furniture.shelf.side_module.generic"},
//                  "xMm": -4295, "yMm": 0, "zMm": -4000, "yawCdeg": 9000, "widthMm": 900, "heightMm": 2000,
//                  "depthMm": 350, "levels": [{"level": 0, "section": "spine", "slots": 33}, ...]}],
//    "placements": [{"slot": "room:snes/fx:w01/s:3.0", "game": "g0123456789abcdef", "insetMm": 2, "yawCdeg": 0}],
//    "dressing": [{"level": "room:snes/fx:w07/l:6", "firstSlot": 0, "count": 33,
//                  "prop": {"kind": "storage_box", "manifest": "decor.storage_box.generic"}}],
//    "lights": [{"id": "room:snes/fx:w01.washer", "role": "wall-washer", "xMm": -3270, "yMm": 2950, "zMm": -3500,
//                "targetXMm": -4230, "targetYMm": 1000, "targetZMm": -3500, "spanMm": 1900}],
//    "unplaced": [{"game": "g...", "room": "room:snes", "reason": "duplicate-title", "representative": "g..."}]}
// Schema 1 has neither "floors" nor a room's "kind", "floor", "sections", "placement", "generatorVersion" and
// "expands", only "room:<system>" rooms and no reason "room-kept". "representative" is there exactly for
// "duplicate-title"; a point light's target is its own position and its span 0 (I3 step 3 added the lights). The lists
// are sorted as sortPlan() sorts them (floors by order then id, rooms and fixtures by id, levels by level, placements
// by slot then game, dressing by level then first slot, lights by id, unplaced by game; a room's sections keep their
// order), and no list holds one key twice (a floor id, an id, a level, a slot with its game, a level with its first
// slot, an unplaced entry, a room's section).
//
// The reader checks the form of every field (types, ranges, ids, enumerations, digests, versions), the order, and what
// a record says of itself: a room's kind is its id's form (console room:<system>, shared area:shared:<floor>, arcade
// area:arcade), a console room holds exactly its own system, a shared room stands on the floor its id names, an
// expansion (and only one) names its chain's head in "expands", the plan's generatorVersion is the highest of its
// rooms'. It does not check how the lists refer to each other (a room's floor among the floors, a placement's fixture
// in the plan, a slot within its level, a game both placed and unplaced): that is the validation's
// (rv/world/validation.hpp), which judges a plan it can read.

#include <map>
#include <set>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <rv/core/expected.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// The plan as its canonical document (its lists sorted, the plan itself untouched). Errors: InvalidArgument when the
/// plan holds what the reader would refuse (an empty id, a field out of its range, a key twice), naming the field, or
/// is not of schema kPlanSchemaVersion (a schema-1 plan is lifted first).
[[nodiscard]] Expected<nlohmann::json> planToJson(const WorldPlan& plan);

/// The canonical text of the plan: world-plan.json's bytes. Errors: those of planToJson().
[[nodiscard]] Expected<std::string> writePlan(const WorldPlan& plan);

/// The plan digest: the SHA-256 of writePlan()'s text, 64 lowercase hexadecimal digits (sha256Hex() of a text already
/// written gives the same). Errors: those of planToJson().
[[nodiscard]] Expected<std::string> planDigest(const WorldPlan& plan);
/// planDigest() of a plan whose lists sortPlan() has sorted already, its text written straight from it (no document, no
/// reader's check; m3/preview-time: the app's preview checks the plan it shows with it). Errors: InvalidArgument for a
/// plan not of schema kPlanSchemaVersion; those of core::writeCanonicalJson() (a string that is not UTF-8).
[[nodiscard]] Expected<std::string> sortedPlanDigest(const WorldPlan& sortedPlan);

/// Reads a plan file's text back: schema 2 as it is, schema 1 as its content (schemaVersion 1, for lift()). Errors
/// (context `context` and the line, "world-plan.json:12"): ParseError (not JSON), ValidationFailed (a text that is not
/// the canonical text of what it holds, a field missing, unknown, of another type or out of its range, a list out of
/// order or holding a key twice, a record that contradicts itself), Unsupported (a schemaVersion newer than
/// kPlanSchemaVersion: written by a newer Retroverse).
[[nodiscard]] Expected<WorldPlan> readPlan(std::string_view text, std::string_view context);

/// A room's content digest (ADR-0013 §5): the SHA-256 of the canonical document of its record and every element of it
/// (its fixtures, placements, dressing, lights and unplaced games), so that two plans' rooms compare without their
/// other rooms. Computed, never stored. Errors: NotFound for a room the plan lacks; those of core::canonicalJsonDigest.
[[nodiscard]] Expected<std::string> roomContentDigest(const WorldPlan& plan, const RoomId& room);
/// Every room's content digest at once (the plan sorted once: what the app's per-room reconcile compares, I4 step 6b).
/// Errors: those of core::canonicalJsonDigest.
[[nodiscard]] Expected<std::map<RoomId, std::string>> roomContentDigests(const WorldPlan& plan);
/// The content digests of `rooms` alone (those the plan holds; I4b step 6's review: a floor's rooms without the
/// whole plan's documents), each equal to roomContentDigests()'s. Errors: those of core::canonicalJsonDigest.
[[nodiscard]] Expected<std::map<RoomId, std::string>> roomContentDigests(const WorldPlan& plan,
                                                                         const std::set<RoomId>& rooms);

namespace detail {

// The digests' canonical texts written straight from the plan and the inputs (m3/preview-time, docs/i4b/preview-time.md):
// a regeneration's digests of a whole world (the plan's, the building's inputs') were most of a room's preview when
// they built a nlohmann::json document first. The texts are the documents' byte for byte; a value the direct writer
// cannot take (a string that is not UTF-8, an input the reader's check would refuse) goes through the document, which
// names the error as before. Public for their tests.

/// writePlan()'s text of a plan whose lists sortPlan() has sorted already, written straight from it (no document, no
/// reader's check). Errors: InvalidArgument for a string that is not UTF-8 (without the field's pointer).
[[nodiscard]] Expected<std::string> writeSortedPlanText(const WorldPlan& sortedPlan);

/// While one is alive on a thread, that thread's digests are made through the documents, as before m3/preview-time
/// (the comparison tests run both paths on every world fixture).
class ScopedDocumentDigests {
public:
    ScopedDocumentDigests() noexcept;
    ~ScopedDocumentDigests();
    ScopedDocumentDigests(const ScopedDocumentDigests&) = delete;
    ScopedDocumentDigests& operator=(const ScopedDocumentDigests&) = delete;
};
/// Whether a ScopedDocumentDigests is alive on this thread.
[[nodiscard]] bool documentDigests() noexcept;

} // namespace detail

} // namespace rv::world
