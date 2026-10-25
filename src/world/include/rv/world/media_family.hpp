#pragma once

// The media family registry (ADR-0010 §2, style §22's MediaFamilyRegistry): assets/world/media_families.json, how the
// games of a family stand on a shelf. A family names its item's prefab and size and the slots it takes spine-out (the
// item on its end, its top face outward: the spine) and face-out (its front outward); the shelves' slot counts follow
// from these through the prefabs' own slot math (ADR-0010 §5), never from constants in the generator. Since I4 step 2
// (ADR-0013 §4) a family also names its item's label quad (its front less the label margin: what its covers are fitted
// to) and the fixtures it stands in.
//
// The file (JSON with comments; every key required unless noted):
//   {"schemaVersion": 1,
//    "families": [{"id": "cartridge_home_16bit",
//                  "item": {"manifest": "media.cartridge.home16.generic", "kind": "cartridge"},
//                  "itemWidthMm": 125, "itemHeightMm": 85, "itemDepthMm": 20,
//                  "labelWidthMm": 101, "labelHeightMm": 61, "coverFit": "layer",
//                  "spineOut": {"widthMm": 20, "heightMm": 125, "depthMm": 85, "gapMm": 4, "marginMm": 30,
//                               "headroomMm": 20},
//                  "faceOut": {"widthMm": 125, "heightMm": 85, "depthMm": 20, "gapMm": 4, "marginMm": 30,
//                              "headroomMm": 20},
//                  "fixtures": {"heroModule": <prefab>, "sideModule": <prefab>, "display": <prefab>,
//                               "wallShelf": <prefab>} (optional, and so is each of its keys)}]}
// The item's size is its manifest's runtime.scaleMeters in millimetres (width, height, depth, the front facing +Z) and
// its label quad the "Label" socket of the prefab its manifest builds, both proven by checkItemManifest(); a spine-out
// slot is the item stood on its end (width = the item's depth, height = its width, depth = its height), a face-out slot
// the item as it is. "coverFit" says what a cover is fitted to: "label", the label quad's shape (ADR-0013 §4), or
// "layer", the front layer's own 256 x 188 (I3's fit, which cartridge_home_16bit keeps: ADR-0013 §4's stated
// exception). The fixtures are the manifests of the hero-wall module and of the side-wall module (kind "shelf"), of the
// display case ("display_case") and of the featured wall shelf ("wall_shelf") at the family's slot sizes; a room of the
// family stands them in the template's sockets instead of the template's own prefabs (withFamilyFixtures()), and a
// fixture the family does not name is the template's. Family ids are permanent (ADR-0013 §4): a family is never
// removed or redefined, a new definition takes a new id.
//
// Since I6 step 4 (ADR-0014 §2) a family may also say (both optional):
//   "spineFacing": "end" (the default: the item on its end, its top face outward, a spine-out slot of (d, w, h)) or
//                  "side" (the item on its side, upright, its -X face outward: geometry::SlotFacing::SideOut, a
//                  spine-out slot of (d, h, w), its manifest's spine label on that face);
//   "succeeds": the id of the family it replaces (another family of the file): a stored room of that family is
//               regenerated once with this one (incremental.hpp's migration), and a new room asked for that family
//               takes this one (successorOf(), latestSuccessor());
//   "succeedsIn": the kinds of room ("console", "shared", "arcade") in which it succeeds that family (optional: every
//                 kind). A family has one successor at most in each kind of room, and the successions of a kind form
//                 no cycle (amended on 2026-10-24: generic_case_v2 succeeds generic_case in console and shared rooms
//                 only, so the arcade room keeps generic_case until arcade_cabinet succeeds it there, ADR-0014 §2);
//   "spineLabelWidthMm", "spineLabelHeightMm" (numbers, to a tenth of a millimetre, both or neither): the item's spine
//                 label quad (its manifest's "Spine" socket, across by along), whose shape a spine picture is drawn on
//                 before its one resample into the 40 x 256 spine layer (spineAspect(), as covers are fitted to the
//                 label quad); without them, the layer's own shape (I3's spines, the cartridges');
//   "shellMaterial": the material of the item's shell (a material of assets/materials; optional: the theme's
//                 "cartridge" role, plastic_shell), e.g. a jewel case's smoky dark plastic or a big box's cardboard.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/assets/cover_fit.hpp>
#include <rv/assets/prefab_manifest.hpp>
#include <rv/core/expected.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// How an item stands in one orientation, in millimetres: the slot it takes (the item's volume in that orientation),
/// the gap between two slots, the margin at each end of a level and the free height kept above the item in its level.
struct SlotSpec {
    std::int64_t widthMm = 0;
    std::int64_t heightMm = 0;
    std::int64_t depthMm = 0;
    std::int64_t gapMm = 0;
    std::int64_t marginMm = 0;
    std::int64_t headroomMm = 0;

    bool operator==(const SlotSpec&) const = default;
};

/// What a family's covers are fitted to (ADR-0013 §4).
enum class CoverFit : std::uint8_t {
    Label, ///< the label quad's shape, then resampled once into the front layer
    Layer, ///< the front layer's own shape (I3's fit: cartridge_home_16bit's stated exception)
};

/// "label", "layer".
[[nodiscard]] constexpr std::string_view toString(CoverFit fit) noexcept {
    switch (fit) {
    case CoverFit::Label:
        return "label";
    case CoverFit::Layer:
        return "layer";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<CoverFit> parseCoverFit(std::string_view text) noexcept {
    return detail::enumFromString(text, CoverFit::Layer);
}

/// The fixtures a family stands in (ADR-0013 §4), each a manifest at its slot size; nothing: the template's own.
struct FamilyFixtures {
    std::optional<PrefabRef> heroModule; ///< kind "shelf": the hero-wall modules
    std::optional<PrefabRef> sideModule; ///< kind "shelf": the side-wall modules
    std::optional<PrefabRef> display;    ///< kind "display_case": the display sockets holding a display case
    std::optional<PrefabRef> wallShelf;  ///< kind "wall_shelf": the display sockets holding a featured wall shelf

    [[nodiscard]] bool empty() const noexcept { return !heroModule && !sideModule && !display && !wallShelf; }

    bool operator==(const FamilyFixtures&) const = default;
};

/// How a family's items stand spine-out (ADR-0014 §2).
enum class SpineFacing : std::uint8_t {
    End,  ///< on the item's end, its top face out (geometry::SlotFacing::SpineOut): I3's cartridges
    Side, ///< on its side, upright, its -X face out (geometry::SlotFacing::SideOut): I6's cases and boxes
};

/// "end", "side".
[[nodiscard]] constexpr std::string_view toString(SpineFacing facing) noexcept {
    switch (facing) {
    case SpineFacing::End:
        return "end";
    case SpineFacing::Side:
        return "side";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<SpineFacing> parseSpineFacing(std::string_view text) noexcept {
    return detail::enumFromString(text, SpineFacing::Side);
}

struct MediaFamily {
    /// A data id ("cartridge_home_16bit").
    std::string id;
    /// The item's prefab.
    PrefabRef item;
    std::int64_t itemWidthMm = 0;
    std::int64_t itemHeightMm = 0;
    std::int64_t itemDepthMm = 0;
    /// The item's label quad: its front less the label margin (the "Label" socket of its prefab).
    std::int64_t labelWidthMm = 0;
    std::int64_t labelHeightMm = 0;
    CoverFit coverFit = CoverFit::Label;
    SlotSpec spineOut;
    SlotSpec faceOut;
    FamilyFixtures fixtures;
    /// How its items stand spine-out (I6 step 4).
    SpineFacing spineFacing = SpineFacing::End;
    /// The family it succeeds (I6 step 4, ADR-0014 §2); empty for none.
    std::string succeeds;
    /// The kinds of room it succeeds that family in, in the file's order; empty: every kind.
    std::vector<RoomKind> succeedsIn;
    /// Its spine label quad in tenths of a millimetre (across, along: the "Spine" socket's size); 0 x 0: none given.
    assets::LabelAspect spineLabel;
    /// The material of its items' shells; empty: the theme's "cartridge" role.
    std::string shellMaterial;

    /// The shape its front pictures are made for (cover_fit.hpp): its label quad, the layer's own for CoverFit::Layer.
    [[nodiscard]] assets::LabelAspect coverAspect() const noexcept;
    /// The shape its spine pictures are made for: its spine label quad, the spine layer's own (I3's 40 x 256) when the
    /// family names none (I6 step 4).
    [[nodiscard]] assets::LabelAspect spineAspect() const noexcept { return spineLabel; }
    /// Whether it succeeds its predecessor in rooms of `kind`.
    [[nodiscard]] bool succeedsInKind(RoomKind kind) const noexcept;

    bool operator==(const MediaFamily&) const = default;
};

struct MediaFamilyRegistry {
    /// In the file's order, no id twice.
    std::vector<MediaFamily> families;

    /// The family of `id`, or nullptr.
    [[nodiscard]] const MediaFamily* find(std::string_view id) const noexcept;
    /// The family that succeeds `id` in rooms of `kind` (its "succeeds" names `id`, its "succeedsIn" `kind`), or
    /// nullptr.
    [[nodiscard]] const MediaFamily* successorOf(std::string_view id, RoomKind kind) const noexcept;
    /// The last of `id`'s successors in rooms of `kind` (following them one by one), `id`'s own family when it has
    /// none, or nullptr for an unknown id.
    [[nodiscard]] const MediaFamily* latestSuccessor(std::string_view id, RoomKind kind) const noexcept;
};

/// Reads the registry. Errors (context and line): ParseError, ValidationFailed (a key missing, unknown, of another type
/// or out of range, an id twice, a slot that is not the item in its orientation (its spine-out one by its spineFacing),
/// a face-out and a spine-out slot alike, a label larger than the item's front, a fixture of another kind than its
/// key's, a "succeeds" that names itself or no family of the file, a family another one succeeds already in a kind of
/// room, successions that form a cycle, a spine label given by half or not to a tenth of a millimetre).
[[nodiscard]] Expected<MediaFamilyRegistry> parseMediaFamilies(std::string_view text, std::string_view context);
/// Reads <assetRoot>/world/media_families.json. Errors: those of parseMediaFamilies() and of reading the file.
[[nodiscard]] Expected<MediaFamilyRegistry> loadMediaFamilies(const std::filesystem::path& assetRoot);

/// The family's item agrees with its manifest: the manifest's id, its runtime.generator equal to the item's kind, its
/// runtime.scaleMeters rounded to whole millimetres equal to the item's size; and the prefab a procedural manifest
/// builds (geometry::buildPrefab with its parameters) as large as the item, its "Label" socket the family's label and,
/// when it has a "Spine" socket, that socket facing out of the face its spineFacing shows (+Y for End, -X for Side)
/// and, when the family names its spine label, as large as that label (to a tenth of a millimetre).
/// Errors: ValidationFailed naming the field (or the build's error).
[[nodiscard]] Status checkItemManifest(const MediaFamily& family, const assets::PrefabManifest& manifest);

/// `roomTemplate` as a room of `family` stands it (ADR-0013 §4): the family's hero-wall and side-wall modules instead
/// of the template's, its display case in every display socket whose prefab is a display case and its wall shelf in
/// every one whose prefab is a wall shelf; what the family does not name stays the template's (all of it for a family
/// without fixtures). The id, the version and the sockets' places are the template's.
[[nodiscard]] RoomTemplate withFamilyFixtures(RoomTemplate roomTemplate, const MediaFamily& family);

} // namespace rv::world
