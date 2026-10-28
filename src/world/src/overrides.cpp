#include <rv/world/overrides.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/time.hpp>
#include <rv/core/utf8.hpp>

#include "data_reader.hpp"

namespace rv::world {
namespace {

using detail::DocumentReader;
using detail::ObjectReader;

/// overrides.json is small: a few pins per placed title at most.
constexpr std::uintmax_t kMaxOverridesBytes = 16ull * 1024ull * 1024ull;
/// The context of writeOverrides()'s errors.
constexpr std::string_view kOverridesWriterContext = "writeOverrides";

/// Whether `slot` is one of the plan's slots: its fixture there, its level listed, its index below the level's count.
bool planHoldsSlot(const std::map<FixtureId, const Fixture*>& fixtures, const SlotId& slot) {
    const auto fixture = fixtures.find(slot.fixture());
    if (fixture == fixtures.end()) {
        return false;
    }
    for (const FixtureLevel& level : fixture->second->levels) {
        if (level.level == slot.level()) {
            return slot.index() < level.slots;
        }
    }
    return false;
}

/// A pin's provenance (schema 2): {"origin": "editor" | "cli" | "file", "time"?: UTC}.
PinProvenance readProvenance(const ObjectReader& pin) {
    PinProvenance provenance;
    if (!pin.has("provenance")) {
        return provenance; // a hand edit: origin file, time unknown
    }
    const ObjectReader reader = pin.object("provenance", {"origin", "time"});
    provenance.origin = reader.enumeration<PinOrigin>("origin", parsePinOrigin, "one of editor, cli, file");
    if (reader.has("time")) {
        provenance.time = reader.checkedString(
            "time", [](std::string_view text) { return core::parseUtcIso8601(text).has_value(); },
            "a UTC time (YYYY-MM-DDTHH:MM:SS.mmmZ)");
    }
    return provenance;
}

/// A fixture pin's transform (schema 2): {"xMm", "zMm", "yawCdeg"}.
FixtureTransform readTransform(const ObjectReader& pin) {
    const ObjectReader reader = pin.object("transform", {"xMm", "yawCdeg", "zMm"});
    FixtureTransform transform;
    transform.xMm = reader.integer("xMm", -kMaxFixtureOffsetMm, kMaxFixtureOffsetMm);
    transform.zMm = reader.integer("zMm", -kMaxFixtureOffsetMm, kMaxFixtureOffsetMm);
    transform.yawCdeg = static_cast<std::int32_t>(reader.integer("yawCdeg", 0, 35999));
    return transform;
}

/// A pin's recorded conflict (schema 2, ADR-0015 §3 as amended on 2026-10-28): "room-gone", "slot-gone", "game-gone"
/// or "invalid"; absent: none.
std::optional<ConflictReason> readConflict(const ObjectReader& pin) {
    if (!pin.has("conflict")) {
        return std::nullopt;
    }
    return pin.enumeration<ConflictReason>("conflict", parseConflictReason,
                                           "one of room-gone, slot-gone, game-gone, invalid");
}

nlohmann::json provenanceJson(const PinProvenance& provenance) {
    nlohmann::json json{{"origin", std::string(toString(provenance.origin))}};
    if (!provenance.time.empty()) {
        json["time"] = provenance.time;
    }
    return json;
}

} // namespace

Expected<WorldOverrides> parseOverrides(std::string_view text, std::string_view context) {
    auto value = detail::parseDataJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    DocumentReader document(text, std::string(context));
    // A newer schema first: whatever else the file holds, this program does not know what it means.
    if (value->is_object()) {
        const auto found = value->find("schemaVersion");
        if (found != value->end() && found->is_number_integer() &&
            found->get<std::int64_t>() > kOverridesSchemaVersion) {
            document.fail("/schemaVersion",
                          "schema " + std::to_string(found->get<std::int64_t>()) +
                              " was written by a newer Retroverse (this one reads " +
                              std::to_string(kOverridesSchemaVersion) + ")",
                          ErrorCode::Unsupported);
            return propagate(document.error());
        }
    }
    const ObjectReader reader(document, *value, "", {"fixturePins", "gamePins", "schemaVersion"});
    // Schema 1 (I3) is lifted: its pins take the default provenance (origin file, time unknown); its keys are I3's.
    const bool lifted = reader.integer("schemaVersion", 1, kOverridesSchemaVersion) == 1;
    WorldOverrides overrides;
    if (reader.has("gamePins")) {
        detail::forEachElement(
            reader.array("gamePins"), reader.pointer("gamePins"),
            [&](const nlohmann::json& element, const std::string& pointer) {
                const ObjectReader pin = lifted
                                             ? ObjectReader(document, element, pointer, {"game", "slot"})
                                             : ObjectReader(document, element, pointer,
                                                            {"conflict", "game", "provenance", "slot"});
                GamePin read;
                read.game = pin.parsed<catalog::GameId>("game", catalog::GameId::parse, "a GameId");
                read.slot = pin.parsed<SlotId>("slot", SlotId::parse, "a slot id");
                read.provenance = readProvenance(pin);
                read.conflict = readConflict(pin);
                overrides.gamePins.push_back(std::move(read));
            });
    }
    if (reader.has("fixturePins")) {
        detail::forEachElement(
            reader.array("fixturePins"), reader.pointer("fixturePins"),
            [&](const nlohmann::json& element, const std::string& pointer) {
                const ObjectReader pin =
                    lifted ? ObjectReader(document, element, pointer, {"fixture", "prefab"})
                           : ObjectReader(document, element, pointer,
                                          {"conflict", "fixture", "prefab", "provenance", "transform"});
                FixturePin read;
                read.fixture = pin.parsed<FixtureId>("fixture", FixtureId::parse, "a fixture id");
                if (pin.has("prefab")) {
                    read.prefab = pin.prefab("prefab");
                }
                if (pin.has("transform")) {
                    read.transform = readTransform(pin);
                }
                read.provenance = readProvenance(pin);
                read.conflict = readConflict(pin);
                overrides.fixturePins.push_back(std::move(read));
            });
    }
    if (document.failed()) {
        return propagate(document.error());
    }
    // One pin per game, per slot and per fixture: two pins of one thing could not both be honoured.
    std::set<catalog::GameId> games;
    std::set<SlotId> slots;
    for (std::size_t i = 0; i < overrides.gamePins.size() && !document.failed(); ++i) {
        const GamePin& pin = overrides.gamePins[i];
        const std::string pointer = detail::pointerChild("/gamePins", i);
        if (!games.insert(pin.game).second) {
            document.fail(pointer, "game " + pin.game.toString() + " is pinned twice");
        } else if (!slots.insert(pin.slot).second) {
            document.fail(pointer, "slot " + pin.slot.toString() + " holds two pins");
        }
    }
    std::set<FixtureId> fixtures;
    for (std::size_t i = 0; i < overrides.fixturePins.size() && !document.failed(); ++i) {
        if (!fixtures.insert(overrides.fixturePins[i].fixture).second) {
            document.fail(detail::pointerChild("/fixturePins", i),
                          "fixture " + overrides.fixturePins[i].fixture.toString() + " is pinned twice");
        }
    }
    if (document.failed()) {
        return propagate(document.error());
    }
    std::sort(overrides.gamePins.begin(), overrides.gamePins.end(),
              [](const GamePin& a, const GamePin& b) { return a.game < b.game; });
    std::sort(overrides.fixturePins.begin(), overrides.fixturePins.end(),
              [](const FixturePin& a, const FixturePin& b) { return a.fixture < b.fixture; });
    return overrides;
}

Expected<WorldOverrides> loadOverrides(const std::filesystem::path& file) {
    auto text = core::readTextFile(file, kMaxOverridesBytes);
    if (!text) {
        if (text.error().code == ErrorCode::NotFound) {
            return WorldOverrides{};
        }
        return propagate(text.error());
    }
    return parseOverrides(*text, core::pathToUtf8Lossy(file.filename()));
}

Expected<std::string> writeOverrides(const WorldOverrides& overrides) {
    std::vector<const GamePin*> gamePins;
    for (const GamePin& pin : overrides.gamePins) {
        gamePins.push_back(&pin);
    }
    std::sort(gamePins.begin(), gamePins.end(), [](const GamePin* a, const GamePin* b) { return a->game < b->game; });
    std::vector<const FixturePin*> fixturePins;
    for (const FixturePin& pin : overrides.fixturePins) {
        fixturePins.push_back(&pin);
    }
    std::sort(fixturePins.begin(), fixturePins.end(),
              [](const FixturePin* a, const FixturePin* b) { return a->fixture < b->fixture; });

    nlohmann::json games = nlohmann::json::array();
    for (const GamePin* pin : gamePins) {
        nlohmann::json json{{"game", pin->game.toString()},
                            {"slot", pin->slot.toString()},
                            {"provenance", provenanceJson(pin->provenance)}};
        if (pin->conflict) {
            json["conflict"] = std::string(toString(*pin->conflict));
        }
        games.push_back(std::move(json));
    }
    nlohmann::json fixtures = nlohmann::json::array();
    for (const FixturePin* pin : fixturePins) {
        nlohmann::json json{{"fixture", pin->fixture.toString()}, {"provenance", provenanceJson(pin->provenance)}};
        if (pin->prefab) {
            json["prefab"] = nlohmann::json{{"manifest", pin->prefab->manifest}, {"kind", pin->prefab->kind}};
        }
        if (pin->transform) {
            json["transform"] = nlohmann::json{
                {"xMm", pin->transform->xMm}, {"zMm", pin->transform->zMm}, {"yawCdeg", pin->transform->yawCdeg}};
        }
        if (pin->conflict) {
            json["conflict"] = std::string(toString(*pin->conflict));
        }
        fixtures.push_back(std::move(json));
    }
    const nlohmann::json document{{"schemaVersion", kOverridesSchemaVersion},
                                  {"gamePins", std::move(games)},
                                  {"fixturePins", std::move(fixtures)}};
    std::string text = document.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    // What is written is read back: overrides the reader would refuse are never written.
    if (auto read = parseOverrides(text, "overrides"); !read) {
        return makeError(ErrorCode::InvalidArgument, "the overrides cannot be written: " + read.error().message,
                         std::string(kOverridesWriterContext));
    }
    return text;
}

PinCheck checkPins(const WorldPlan& plan, const std::vector<RoomTemplate>& templates,
                   const WorldOverrides& overrides) {
    PinCheck check;
    std::map<FixtureId, const Fixture*> fixtures;
    for (const Fixture& fixture : plan.fixtures) {
        fixtures.emplace(fixture.id, &fixture);
    }
    std::map<catalog::GameId, const SlotId*> placed;
    for (const Placement& placement : plan.placements) {
        placed.emplace(placement.game, &placement.slot);
    }
    std::set<catalog::GameId> unplaced;
    for (const UnplacedGame& game : plan.unplaced) {
        unplaced.insert(game.game);
    }
    for (const GamePin& pin : overrides.gamePins) {
        const auto stands = placed.find(pin.game);
        if (stands != placed.end() && *stands->second == pin.slot) {
            check.honouredOrOrphaned.gamePins.push_back(pin);
            check.honoured.gamePins.push_back(pin);
        } else if (!planHoldsSlot(fixtures, pin.slot) || (stands == placed.end() && !unplaced.contains(pin.game))) {
            check.orphaned.push_back(pinText(pin));
            check.honouredOrOrphaned.gamePins.push_back(pin);
        } else if (pin.conflict) {
            check.orphaned.push_back(pinText(pin)); // a regeneration's conflict: reported, not judged
        } else {
            check.unhonoured.push_back(pinText(pin));
        }
    }
    for (const FixturePin& pin : overrides.fixturePins) {
        const Room* room = nullptr;
        for (const Room& candidate : plan.rooms) {
            if (candidate.id == pin.fixture.room()) {
                room = &candidate;
            }
        }
        const RoomTemplate* roomTemplate = nullptr;
        for (const RoomTemplate& candidate : templates) {
            if (room != nullptr && candidate.id == room->templateId) {
                roomTemplate = &candidate;
            }
        }
        const auto fixture = fixtures.find(pin.fixture);
        const auto standsAsPinned = [&](const Fixture& standing) {
            return (!pin.prefab || *pin.prefab == standing.prefab) &&
                   (!pin.transform || (pin.transform->xMm == standing.xMm && pin.transform->zMm == standing.zMm &&
                                       pin.transform->yawCdeg == standing.yawCdeg));
        };
        if (fixture != fixtures.end() && standsAsPinned(*fixture->second)) {
            check.honouredOrOrphaned.fixturePins.push_back(pin);
            check.honoured.fixturePins.push_back(pin);
        } else if (roomTemplate == nullptr || roomTemplate->socket(pin.fixture.socket()) == nullptr) {
            check.orphaned.push_back(pinText(pin));
            check.honouredOrOrphaned.fixturePins.push_back(pin);
        } else if (pin.conflict) {
            check.orphaned.push_back(pinText(pin)); // a regeneration's conflict: reported, not judged
        } else {
            check.unhonoured.push_back(pinText(pin));
        }
    }
    std::sort(check.orphaned.begin(), check.orphaned.end());
    std::sort(check.unhonoured.begin(), check.unhonoured.end());
    return check;
}

std::string pinText(const GamePin& pin) {
    return "game " + pin.game.toString() + " pinned to " + pin.slot.toString();
}

std::string pinText(const FixturePin& pin) {
    std::string text =
        "fixture " + pin.fixture.toString() + " pinned" + (pin.prefab ? " as " + pin.prefab->manifest : "");
    if (pin.transform) {
        text += " at x " + std::to_string(pin.transform->xMm) + " z " + std::to_string(pin.transform->zMm) +
                " mm, yaw " + std::to_string(pin.transform->yawCdeg) + " cdeg";
    }
    return text;
}

} // namespace rv::world
