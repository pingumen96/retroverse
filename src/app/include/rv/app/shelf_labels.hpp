#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// The shelves' detail (I12 step 5, ADR-0021 §1): what a room's letter tabs, caption cards and section signs say and
// where they stand, from its collection's layout, its plan and its items' text. None of it is part of the plan or of
// the scene files: the app adds them to the scene shown at runtime (WorldShelfLabels: SceneInstance::replaceObjects()
// under kTabsPrefix, kCaptionsPrefix and kSectionSignPrefix), draws their atlases on the pool and lays them on the
// objects' pictures, as the plaques are; a section sign takes the identity of its system through WorldSigns.
//
//   - A letter tab on every spine-out level of a module (a fixture of prefab kind "shelf") that holds an item whose
//     initial is known: a brass holder of kTabSize round a paper insert, at the level's board's left end, hanging over
//     its front edge (the prefab's "Level_<n>" anchor: its top, its left end and its front edge), saying the level's
//     first and last initials (letterRun(): "A–C", "D", "0-9", "#"). The initials are the sort keys' (shelfInitial()):
//     the level's least and greatest by initialRank(), so that an editor's move never makes a run read backwards.
//   - A caption card under every face-out item of a display case (kind "display_case"): kCaptionSize (narrower when the
//     slots stand closer), hanging from the lip of the item's ledge, leaning with it (the slot socket's frame), saying
//     the title and "year · developer" (captionDetail()).
//   - A section sign over the first fixture of each section of a shared or arcade room: the module, wall shelf or
//     display case holding the section's first item by sort key (a small shared room holds every title face-out, in its
//     display cases and its wall shelf); a plate of kSectionSignSize standing on the fixture's top, at a module's or
//     the wall shelf's front and at a display case's back (behind its glass), sharing the fixture's width with the
//     other sections that begin there; its generic picture the section's name (the hall's sign plate's lettering),
//     WorldSigns putting its system's logo, else its identity's name, over it.
//
// All of it in the scene's frame, as the floor's collection layouts are (world::floorCollectionLayout()).

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

#include <rv/app/room_items.hpp>
#include <rv/assets/label_atlas.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::app {

/// The objects' ids: "tabs:<room>/plates", "captions:<room>/plates", "section:<room>/<section index>"; a room's under
/// labelsPrefix() ("tabs:room:snes/"), which no other room's ids begin with.
inline constexpr std::string_view kTabsPrefix = "tabs:";
inline constexpr std::string_view kCaptionsPrefix = "captions:";
inline constexpr std::string_view kSectionSignPrefix = "section:";

/// A tab's holder (66 x 26 mm, 3 mm thick) and the brass round its insert (its cell: assets::kTabCell's 3 to 1); its
/// top stands kTabRise over the board's top, its back kPlateGap in front of the board's edge, kTabFromEnd from the
/// board's left end.
inline constexpr glm::vec2 kTabSize{0.066F, 0.026F};
inline constexpr float kTabInset = 0.003F;
inline constexpr float kTabRise = 0.003F;
inline constexpr float kTabFromEnd = 0.006F;
/// A caption card (75 x 28 mm, assets::kCaptionCell's shape), its top kCaptionRise over the lip's; narrower, at the
/// same shape, when the slots' pitch less kCaptionGap is less.
inline constexpr glm::vec2 kCaptionSize{0.075F, 0.028F};
inline constexpr float kCaptionRise = 0.002F;
inline constexpr float kCaptionGap = 0.004F;
/// The plates' thickness and their gap to what they hang on.
inline constexpr float kPlateThickness = 0.003F;
inline constexpr float kPlateGap = 0.0005F;
/// A section sign: at most 50 x 14 cm, 2 cm deep in a frame of 1.2 cm; kSectionSignGap between two on one module.
inline constexpr glm::vec2 kSectionSignSize{0.50F, 0.14F};
inline constexpr float kSectionSignDepth = 0.02F;
inline constexpr float kSectionSignFrame = 0.012F;
inline constexpr float kSectionSignGap = 0.04F;

/// The initial a sort key files under: its first character, an ASCII letter in upper case ("A" to "Z"), "0-9" for a
/// digit, "#" for anything else and for an empty key.
[[nodiscard]] std::string shelfInitial(std::string_view sortKey);
/// The order of initials: "#" first, then "0-9", then "A" to "Z" (anything else after them).
[[nodiscard]] int initialRank(std::string_view initial) noexcept;
/// A level's tab: `first` when it is `last`, else "<first>–<last>" (an en dash; the digits "0" within a run: "0–C").
[[nodiscard]] std::string letterRun(std::string_view first, std::string_view last);
/// A caption's second line: "<year> · <maker>", the maker the developer, else the publisher; the one known alone; "".
[[nodiscard]] std::string captionDetail(std::string_view year, std::string_view developer, std::string_view publisher);

/// What the labels say of a placed game: its title, its caption's second line, its sort key and initial, its section
/// (an index into its room's sections; none when no section's items name it).
struct ShelfText {
    catalog::GameId game;
    std::string title;
    std::string detail;
    std::string sortKey;
    std::string initial;
    std::optional<std::size_t> section;

    bool operator==(const ShelfText&) const = default;
};

/// `text`'s (its title, else its file name's; its screen data's sort key, else the title's catalog::sortKeyOf()).
[[nodiscard]] ShelfText shelfTextOf(const RoomItemText& text, std::optional<std::size_t> section);

/// What a room's labels need besides its layout: its games' text (by GameId, each once) and its sections' names (the
/// systems' full names as their items give them, by section; "" unknown).
struct RoomShelfTexts {
    std::vector<ShelfText> games;
    std::vector<std::string> sectionNames;

    /// The text of `game`, or nullptr.
    [[nodiscard]] const ShelfText* find(const catalog::GameId& game) const noexcept;

    bool operator==(const RoomShelfTexts&) const = default;
};

/// A level's tab: its fixture ("fx:w01"), its level, its text and its holder's frame (its back's centre, +Z out of the
/// insert, +Y up) in the scene.
struct ShelfTab {
    world::FixtureId fixture;
    std::uint32_t level = 0;
    std::string text;
    glm::mat4 frame{1.0F};

    bool operator==(const ShelfTab&) const = default;
};

/// A face-out item's caption card: its fixture, its game, what it says, its frame and its size.
struct ShelfCaption {
    world::FixtureId fixture;
    catalog::GameId game;
    assets::CaptionText text;
    glm::mat4 frame{1.0F};
    glm::vec2 size = kCaptionSize;

    bool operator==(const ShelfCaption&) const = default;
};

/// A section's sign: its index among the room's sections, its system, its generic picture's text (the section's name),
/// its transform in the scene (a poster frame's: the back of its frame at its centre) and its size.
struct ShelfSectionSign {
    std::size_t section = 0;
    catalog::SystemName system;
    std::string name;
    glm::mat4 transform{1.0F};
    glm::vec2 size = kSectionSignSize;

    bool operator==(const ShelfSectionSign&) const = default;
};

/// A room's labels, in the scene's frame, and the pack's materials for them: the tabs' holder and insert
/// ("label_plates"' "plate" and "picture"), the cards' paper (the insert's), the signs' frame and plate (the pack's
/// hall's "trim" and "sign").
struct ShelfLabels {
    world::RoomId room;
    std::vector<ShelfTab> tabs;
    std::vector<ShelfCaption> captions;
    std::vector<ShelfSectionSign> sections;
    std::map<std::string, std::string> tabMaterials;
    std::string cardMaterial;
    std::map<std::string, std::string> signMaterials;

    [[nodiscard]] bool empty() const noexcept { return tabs.empty() && captions.empty() && sections.empty(); }

    bool operator==(const ShelfLabels&) const = default;
};

/// The labels of `room` of `plan` (see the file comment): `layout` its collection in the scene's frame, its room
/// standing at `scenePlacement` there, `data` its scene data (its fixtures' prefabs built, its theme), `texts` its
/// games' text. Fixtures whose prefab `data` lacks or did not build are left out; at most assets::kMaxAtlasCells tabs
/// and captions each (the first in the layout's order).
[[nodiscard]] ShelfLabels roomShelfLabels(const world::WorldPlan& plan, const world::RoomId& room,
                                          const world::SceneData& data, const world::CollectionLayout& layout,
                                          const world::RoomPlacement& scenePlacement, const RoomShelfTexts& texts);

/// The objects: "tabs:<room>" and "captions:<room>" (label_plates, each plate's cell from `layout`, the atlas the
/// labels' texts are drawn in: assets::labelAtlasLayout() with assets::kTabCell or kCaptionCell), and a poster frame
/// per section sign; none for no tab, no caption. Their pictures have a generic pattern of their own (a material per
/// object, for the drawn atlas or the identity's sign), none casts a shadow or collides.
[[nodiscard]] std::optional<scene::SceneObject> shelfTabsObject(const ShelfLabels& labels);
[[nodiscard]] std::optional<scene::SceneObject> shelfCaptionsObject(const ShelfLabels& labels);
[[nodiscard]] scene::SceneObject sectionSignObject(const ShelfLabels& labels, const ShelfSectionSign& sign);

/// The ids: "tabs:room:snes", "captions:room:snes", "section:area:shared:handheld/1"; and the room and section a
/// section sign's id names.
[[nodiscard]] std::string labelsPrefix(std::string_view kindPrefix, std::string_view room);
[[nodiscard]] std::string shelfTabsId(const world::RoomId& room);
[[nodiscard]] std::string shelfCaptionsId(const world::RoomId& room);
[[nodiscard]] std::string sectionSignId(const world::RoomId& room, std::size_t section);
[[nodiscard]] std::optional<std::pair<world::RoomId, std::size_t>> sectionSignOf(std::string_view objectId);

/// The texts the atlases are drawn from, in their cells' order.
[[nodiscard]] std::vector<std::string> tabTexts(const ShelfLabels& labels);
[[nodiscard]] std::vector<assets::CaptionText> captionTexts(const ShelfLabels& labels);

} // namespace rv::app
