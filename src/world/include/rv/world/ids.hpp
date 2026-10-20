#pragma once

// The world's identifiers (ADR-0010 §7, ADR-0013 §5). They name what the template, the building and the catalog name,
// never a random choice of the generator, so that a plan's ids are the same whatever the seed:
//   - RoomId, a tagged id of four forms:
//       "room:<system>"            a system's console room ("room:snes"; RoomId::forSystem());
//       "room:<system>:<n>"        an expansion of it, n from 2 (ADR-0013 §8: reserved, nothing generates one yet);
//       "area:shared:<floor>[:<n>]" the shared room of a floor ("area:shared:gen4"), its expansions from 2;
//       "area:arcade[:<n>]"        the arcade game room on the ground floor, its expansions from 2.
//     The system is an ES-DE system name (catalog::SystemName, which never holds ':' or '/'), the floor a data id
//     ("gen4": building.json's), n a decimal number from 2 to kMaxRoomIndex without leading zeros; the head of a chain
//     has no number (index 1). A room's seed derives from its id's text (seeds.hpp), so "room:snes" keeps its seed;
//   - FixtureId: the room, "/fx:" and the template's socket ("room:snes/fx:w03", "room:snes/fx:dc01"), with "." and a
//     part when a socket holds several fixtures ("room:snes/fx:hero.crt");
//   - LevelId: a fixture, "/l:" and a level of its slots, 0 the lowest ("room:snes/fx:w03/l:2"): a dressed level;
//   - SlotId: a fixture, "/s:", its level, "." and the slot's index in it, 0 the leftmost as one faces the fixture
//     ("room:snes/fx:w03/s:3.12"), as the prefab's "Slot_<level>_<index>" socket names it.
// Fixtures, levels and slots work alike in every form of room ("area:shared:gen4/fx:w03/s:3.12").
// Socket and part names are 1 to 32 characters among lowercase ASCII letters, digits and '-', starting with a letter;
// levels run from 0 to kMaxLevel and indices from 0 to kMaxSlotIndex, written in decimal without leading zeros. Every
// id is validated when it is made or parsed, so a value of these types is always well-formed (a default-constructed one
// is empty and stands for "none"), and its text can enter a seed's tuple (no NUL, well-formed UTF-8). Ids order by
// their parts, numbers as numbers: rooms by form (area:arcade, area:shared, room), then floor or system, then index;
// fixtures by room, socket, part; "s:2.0" before "s:10.0".

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>

namespace rv::world {

inline constexpr std::size_t kMaxSocketNameLength = 32;
inline constexpr std::uint32_t kMaxLevel = 999;
inline constexpr std::uint32_t kMaxSlotIndex = 9999;

/// A socket or part name: 1 to kMaxSocketNameLength characters among a-z, 0-9 and '-', the first a letter.
[[nodiscard]] bool isValidSocketName(std::string_view text) noexcept;

/// The largest number of a room of a chain ("room:snes:999").
inline constexpr std::uint32_t kMaxRoomIndex = 999;

/// What a room id names (its form).
enum class RoomIdKind : std::uint8_t {
    Arcade, ///< "area:arcade[:<n>]"
    Shared, ///< "area:shared:<floor>[:<n>]"
    System, ///< "room:<system>[:<n>]"
};

/// A room of the world (the file's comment: four forms).
class RoomId {
public:
    RoomId() = default;

    /// The console room of `system`, "room:<system>". Errors: InvalidArgument for "no system".
    [[nodiscard]] static Expected<RoomId> forSystem(const catalog::SystemName& system);
    /// Room `index` of `system`'s chain: 1 the head ("room:<system>"), from 2 an expansion ("room:<system>:<n>").
    /// Errors: InvalidArgument for "no system" or an index outside 1 to kMaxRoomIndex.
    [[nodiscard]] static Expected<RoomId> forSystem(const catalog::SystemName& system, std::uint32_t index);
    /// The shared room of `floor` (a data id), "area:shared:<floor>[:<n>]". Errors: InvalidArgument for a floor that
    /// is not a data id or an index outside 1 to kMaxRoomIndex.
    [[nodiscard]] static Expected<RoomId> sharedArea(std::string_view floor, std::uint32_t index = 1);
    /// The arcade game room, "area:arcade[:<n>]". Errors: InvalidArgument for an index outside 1 to kMaxRoomIndex.
    [[nodiscard]] static Expected<RoomId> arcadeArea(std::uint32_t index = 1);
    /// Any of the four forms. Errors: InvalidArgument naming the text.
    [[nodiscard]] static Expected<RoomId> parse(std::string_view text);

    [[nodiscard]] RoomIdKind kind() const noexcept { return m_kind; }
    /// The system of a "room:" id; empty for an area.
    [[nodiscard]] const catalog::SystemName& system() const noexcept { return m_system; }
    /// The floor of a shared room; empty otherwise.
    [[nodiscard]] const std::string& floor() const noexcept { return m_floor; }
    /// The room's place in its chain: 1 the head, from 2 an expansion.
    [[nodiscard]] std::uint32_t index() const noexcept { return m_index; }
    [[nodiscard]] bool isExpansion() const noexcept { return m_index > 1; }
    /// The head of the room's chain (the room itself when it is one).
    [[nodiscard]] RoomId head() const;
    [[nodiscard]] bool empty() const noexcept { return m_kind == RoomIdKind::System && m_system.empty(); }
    /// "room:snes", "room:snes:2", "area:shared:gen4", "area:arcade"; empty for "no room".
    [[nodiscard]] std::string toString() const;

    bool operator==(const RoomId&) const = default;
    /// By form (empty first, then area:arcade, area:shared, room), then floor or system, then index.
    std::strong_ordering operator<=>(const RoomId& other) const;

private:
    RoomIdKind m_kind = RoomIdKind::System;
    catalog::SystemName m_system;
    std::string m_floor;
    std::uint32_t m_index = 1;
};

/// A fixture of a room: what stands in one of its template's sockets, or one part of it.
class FixtureId {
public:
    FixtureId() = default;

    /// Errors: InvalidArgument for "no room", a socket name or a part name (when not empty) that is not valid.
    [[nodiscard]] static Expected<FixtureId> make(const RoomId& room, std::string_view socket,
                                                  std::string_view part = {});
    /// "<room>/fx:<socket>" or "<room>/fx:<socket>.<part>". Errors: InvalidArgument naming the text.
    [[nodiscard]] static Expected<FixtureId> parse(std::string_view text);

    [[nodiscard]] const RoomId& room() const noexcept { return m_room; }
    [[nodiscard]] const std::string& socket() const noexcept { return m_socket; }
    /// Empty for a socket's only fixture.
    [[nodiscard]] const std::string& part() const noexcept { return m_part; }
    [[nodiscard]] bool empty() const noexcept { return m_socket.empty(); }
    /// The id within its room: "fx:w03", "fx:hero.crt".
    [[nodiscard]] std::string localText() const;
    [[nodiscard]] std::string toString() const;

    auto operator<=>(const FixtureId&) const = default;

private:
    RoomId m_room;
    std::string m_socket;
    std::string m_part;
};

/// A level of a fixture's slots (a dressed level).
class LevelId {
public:
    LevelId() = default;

    /// Errors: InvalidArgument for "no fixture" or a level above kMaxLevel.
    [[nodiscard]] static Expected<LevelId> make(const FixtureId& fixture, std::uint32_t level);
    /// "<fixture>/l:<level>". Errors: InvalidArgument naming the text.
    [[nodiscard]] static Expected<LevelId> parse(std::string_view text);

    [[nodiscard]] const FixtureId& fixture() const noexcept { return m_fixture; }
    [[nodiscard]] std::uint32_t level() const noexcept { return m_level; }
    [[nodiscard]] bool empty() const noexcept { return m_fixture.empty(); }
    /// The level within its room, as ADR-0010 §7 names a dressed level's socket in its prop seed: "fx:w03/l:2".
    [[nodiscard]] std::string localText() const;
    [[nodiscard]] std::string toString() const;

    auto operator<=>(const LevelId&) const = default;

private:
    FixtureId m_fixture;
    std::uint32_t m_level = 0;
};

/// A slot of a fixture: where one game stands.
class SlotId {
public:
    SlotId() = default;

    /// Errors: InvalidArgument for "no fixture", a level above kMaxLevel or an index above kMaxSlotIndex.
    [[nodiscard]] static Expected<SlotId> make(const FixtureId& fixture, std::uint32_t level, std::uint32_t index);
    /// "<fixture>/s:<level>.<index>". Errors: InvalidArgument naming the text.
    [[nodiscard]] static Expected<SlotId> parse(std::string_view text);

    [[nodiscard]] const FixtureId& fixture() const noexcept { return m_fixture; }
    [[nodiscard]] std::uint32_t level() const noexcept { return m_level; }
    [[nodiscard]] std::uint32_t index() const noexcept { return m_index; }
    [[nodiscard]] bool empty() const noexcept { return m_fixture.empty(); }
    /// The level the slot belongs to.
    [[nodiscard]] LevelId levelId() const;
    [[nodiscard]] std::string toString() const;

    auto operator<=>(const SlotId&) const = default;

private:
    FixtureId m_fixture;
    std::uint32_t m_level = 0;
    std::uint32_t m_index = 0;
};

/// A prefab as the world names it (specification §17.3): its manifest id (assets::isValidPrefabId: "furniture.
/// display_case.generic") and the procedural kind the manifest is expected to build ("display_case"), never a model
/// path. The kind lets the scene fall back to the family's generic prefab of that kind when the manifest is missing.
struct PrefabRef {
    std::string manifest;
    std::string kind;

    auto operator<=>(const PrefabRef&) const = default;
};

/// A prefab kind: 1 to 64 characters among a-z, 0-9 and '_', the first a letter ("shelf", "tv_cabinet").
[[nodiscard]] bool isValidPrefabKind(std::string_view text) noexcept;

/// A preset, template, theme pack, media family or platform id: 1 to 64 characters among a-z, 0-9 and '_', the first
/// a letter ("retro_museum_cozy", "console_room_9x10", "cartridge_home_16bit").
[[nodiscard]] bool isValidDataId(std::string_view text) noexcept;

/// A versioned rule name: a data id, '/', and a version from 1 without leading zeros ("splitmix64/1",
/// "title-key/1"; '-' is allowed in the name).
[[nodiscard]] bool isValidRuleName(std::string_view text) noexcept;

namespace detail {

/// The value of an enumeration of the world whose toString() is `text`; its values run from 0 to `last`.
template <class Enum>
[[nodiscard]] constexpr std::optional<Enum> enumFromString(std::string_view text, Enum last) noexcept {
    for (int value = 0; value <= static_cast<int>(last); ++value) {
        if (toString(static_cast<Enum>(value)) == text) {
            return static_cast<Enum>(value);
        }
    }
    return std::nullopt;
}

/// A decimal number from 0 to `maximum` without leading zeros ("0", "12"); nothing otherwise.
[[nodiscard]] std::optional<std::uint32_t> parseDecimal(std::string_view text, std::uint32_t maximum) noexcept;

} // namespace detail

} // namespace rv::world
