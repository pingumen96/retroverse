#include <rv/world/overrides.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include <rv/core/file_io.hpp>
#include <rv/core/utf8.hpp>

#include "data_reader.hpp"

namespace rv::world {
namespace {

using detail::DocumentReader;
using detail::ObjectReader;

/// overrides.json is small: a few pins per placed title at most.
constexpr std::uintmax_t kMaxOverridesBytes = 16ull * 1024ull * 1024ull;

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
    (void)reader.integer("schemaVersion", kOverridesSchemaVersion, kOverridesSchemaVersion);
    WorldOverrides overrides;
    if (reader.has("gamePins")) {
        detail::forEachElement(reader.array("gamePins"), reader.pointer("gamePins"),
                               [&](const nlohmann::json& element, const std::string& pointer) {
                                   const ObjectReader pin(document, element, pointer, {"game", "slot"});
                                   GamePin read;
                                   read.game = pin.parsed<catalog::GameId>("game", catalog::GameId::parse, "a GameId");
                                   read.slot = pin.parsed<SlotId>("slot", SlotId::parse, "a slot id");
                                   overrides.gamePins.push_back(std::move(read));
                               });
    }
    if (reader.has("fixturePins")) {
        detail::forEachElement(reader.array("fixturePins"), reader.pointer("fixturePins"),
                               [&](const nlohmann::json& element, const std::string& pointer) {
                                   const ObjectReader pin(document, element, pointer, {"fixture", "prefab"});
                                   FixturePin read;
                                   read.fixture = pin.parsed<FixtureId>("fixture", FixtureId::parse, "a fixture id");
                                   if (pin.has("prefab")) {
                                       read.prefab = pin.prefab("prefab");
                                   }
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
        } else if (!planHoldsSlot(fixtures, pin.slot) || (stands == placed.end() && !unplaced.contains(pin.game))) {
            check.orphaned.push_back(pinText(pin));
            check.honouredOrOrphaned.gamePins.push_back(pin);
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
        if (fixture != fixtures.end() && (!pin.prefab || *pin.prefab == fixture->second->prefab)) {
            check.honouredOrOrphaned.fixturePins.push_back(pin);
        } else if (roomTemplate == nullptr || roomTemplate->socket(pin.fixture.socket()) == nullptr) {
            check.orphaned.push_back(pinText(pin));
            check.honouredOrOrphaned.fixturePins.push_back(pin);
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
    return "fixture " + pin.fixture.toString() + " pinned" + (pin.prefab ? " as " + pin.prefab->manifest : "");
}

} // namespace rv::world
