#pragma once

// The WorldPlan's canonical JSON (ADR-0010 §8): the file world-plan.json holds exactly writePlan()'s text, written with
// core::writeCanonicalJson (keys by code point, integers only, one element per line in the top-level arrays, a final
// line feed), so that one plan is one text in Debug and Release and its SHA-256 is the plan digest. The reader takes
// back only such a text: a plan edited by hand or written by another writer is refused, not normalized, and so is a
// plan of a newer schema.
//
// The document (every key required unless noted):
//   {"catalogSortKeyVersion": 1, "generatorVersion": "1.0", "inputDigest": "<64 hex>", "library": "esde",
//    "preset": "retro_museum_cozy", "randomAlgorithm": "splitmix64/1", "schemaVersion": 1, "seed": <0..2^64-1>,
//    "titleKeyRule": "title-key/1",
//    "rooms": [{"id": "room:snes", "template": "console_room_9x10", "templateVersion": 1, "minXMm": -4500,
//               "maxXMm": 4500, "minZMm": -5000, "maxZMm": 5000, "heightMm": 3200, "growthSteps": 0, "seed": <u64>,
//               "mediaFamily": "cartridge_home_16bit", "themePack": "classic_16bit_warm"}],
//    "fixtures": [{"id": "room:snes/fx:w01", "role": "side-module",
//                  "prefab": {"kind": "shelf", "manifest": "furniture.shelf.side_module.generic"},
//                  "xMm": -4295, "yMm": 0, "zMm": -4000, "yawCdeg": 9000, "widthMm": 900, "heightMm": 2000,
//                  "depthMm": 350, "levels": [{"level": 0, "section": "spine", "slots": 33}, ...]}],
//    "placements": [{"slot": "room:snes/fx:w01/s:3.0", "game": "g0123456789abcdef", "insetMm": 2, "yawCdeg": 0}],
//    "dressing": [{"level": "room:snes/fx:w07/l:6", "firstSlot": 0, "count": 33,
//                  "prop": {"kind": "storage_box", "manifest": "decor.storage_box.generic"}}],
//    "unplaced": [{"game": "g...", "room": "room:snes", "reason": "duplicate-title", "representative": "g..."}]}
// "representative" is there exactly for "duplicate-title". The lists are sorted as sortPlan() sorts them (rooms and
// fixtures by id, levels by level, placements by slot then game, dressing by level then first slot, unplaced by game),
// and no list holds one key twice (an id, a level, a slot with its game, a level with its first slot, an unplaced
// entry).
//
// The reader checks the form of every field (types, ranges, ids, enumerations, digests, versions) and the order; it
// does not check how the lists refer to each other (a placement's fixture in the plan, a slot within its level, a game
// both placed and unplaced): that is the validation's (rv/world/validation.hpp, I3 step 4), which judges a plan it can
// read.

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <rv/core/expected.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// The plan as its canonical document (its lists sorted, the plan itself untouched). Errors: InvalidArgument when the
/// plan holds what the reader would refuse (an empty id, a field out of its range, a key twice), naming the field.
[[nodiscard]] Expected<nlohmann::json> planToJson(const WorldPlan& plan);

/// The canonical text of the plan: world-plan.json's bytes. Errors: those of planToJson().
[[nodiscard]] Expected<std::string> writePlan(const WorldPlan& plan);

/// The plan digest: the SHA-256 of writePlan()'s text, 64 lowercase hexadecimal digits (sha256Hex() of a text already
/// written gives the same). Errors: those of planToJson().
[[nodiscard]] Expected<std::string> planDigest(const WorldPlan& plan);

/// Reads a plan file's text back. Errors (context `context` and the line, "world-plan.json:12"): ParseError (not JSON),
/// ValidationFailed (a text that is not the canonical text of what it holds, a field missing, unknown, of another type
/// or out of its range, a list out of order or holding a key twice), Unsupported (a schemaVersion newer than
/// kPlanSchemaVersion: written by a newer Retroverse).
[[nodiscard]] Expected<WorldPlan> readPlan(std::string_view text, std::string_view context);

} // namespace rv::world
