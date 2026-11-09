#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or RmlUi header is included.
//
// The gallery's pictures (I12 step 2, ADR-0021 §2): which of the user's own media each frame of the scene shows. The
// scene builder hangs the frames with the pack's print in them (world/gallery.hpp: "dressing:gallery:<fixture>", in a
// floor scene under its room's prefix); the app puts a game's picture in each (app's WorldGallery):
//   - a room's candidates (roomGallery()): its placed games in the room's priority order (world::priorityLess:
//     favourites, play count, rating, sort key), each with the picture a frame of each kind takes of it, from the
//     rooms' items (room_items.hpp, schema 3): a poster its fanart, a portrait its 3D box (else its cover), a lightbox
//     its marquee, a frame its screenshot (else its title screen, else its miximage);
//   - the scene's frames (galleryTargets()), each's kind read back from its picture's aspect;
//   - the choice (chooseGalleryPictures()): the frames of a room in the scene's order, each taking, among the unused
//     candidates with a picture of its kind, one drawn by core::deriveSeed(the room's seed, "gallery-game", its
//     fixture) from the twice-as-many-as-frames first ones (else from all); a game at most once a room; a frame no
//     candidate fits keeps its print;
//   - the picture (makeGalleryPicture()): the medium decoded upright, never stretched: fitted inside the frame's
//   picture
//     on a mat of its own dominant colour darkened (a frame's screenshot on a mat all round), at
//     galleryUploadSize().

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec2.hpp>

#include <rv/app/room_items.hpp>
#include <rv/assets/image_data.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/gallery.hpp>
#include <rv/world/ids.hpp>

namespace rv::app {

/// A placed game the gallery may show, with the picture each kind of frame takes of it.
struct GalleryCandidate {
    catalog::GameId game;
    /// By world::GalleryKind: the medium under the media root (empty: the game has none for that kind).
    std::array<std::filesystem::path, 4> media;

    bool operator==(const GalleryCandidate&) const = default;
};

/// One room's gallery: its seed (the plan's) and its candidates in its priority order.
struct RoomGallery {
    std::uint64_t seed = 0;
    std::vector<GalleryCandidate> candidates;

    bool operator==(const RoomGallery&) const = default;
};

/// The item of a placed game, among the rooms' items (null: none).
using GalleryItemLookup = std::function<const RoomItemText*(const catalog::GameId& game)>;

/// `layout`'s placed games whose item (`itemOf`) has a picture for some kind, their media under `mediaRoot`, in the
/// room's priority order (the games without the item's screen data, which holds the priority, are left out).
[[nodiscard]] RoomGallery roomGallery(const world::CollectionLayout& layout, const GalleryItemLookup& itemOf,
                                      const std::filesystem::path& mediaRoot, std::uint64_t seed);

/// A frame of the scene.
struct GalleryTarget {
    std::string objectId;
    /// Its room (the floor scene's prefix "<room id>/"; empty in a scene of one room without prefixes).
    std::string room;
    /// Its fixture's local id ("fx:w01").
    std::string fixture;
    world::GalleryKind kind = world::GalleryKind::Poster;
};

/// The frames of `description` (the gallery's poster_frame objects, not their hoods and washes), in its object order.
[[nodiscard]] std::vector<GalleryTarget> galleryTargets(const scene::SceneDescription& description);

/// A frame's picture chosen.
struct GalleryChoice {
    std::string objectId;
    catalog::GameId game;
    std::filesystem::path media;
    world::GalleryKind kind = world::GalleryKind::Poster;

    bool operator==(const GalleryChoice&) const = default;
};

/// The pictures of `targets` (one room's frames, the scene's order) from `gallery` (see the file comment): one choice
/// per frame that has one, in the frames' order.
[[nodiscard]] std::vector<GalleryChoice> chooseGalleryPictures(std::span<const GalleryTarget> targets,
                                                               const RoomGallery& gallery);

/// The pixels a frame's picture is made at: poster 1024 x 576, portrait 640 x 800, lightbox 1024 x 344, frame 768 x 576
/// (each a multiple of 4, for its blocks).
[[nodiscard]] glm::ivec2 galleryUploadSize(world::GalleryKind kind) noexcept;

/// The mat's share of a frame's picture on each side (a screenshot framed; the other kinds take the mat only where the
/// medium's aspect leaves it).
inline constexpr float kGalleryFrameMat = 0.07f;
/// The mat's colour: the dominant colour of the medium's opaque pixels times this, in linear light (a lightbox's
/// kGalleryLightboxMat, nearly black, so that a marquee's lettering glows on it).
inline constexpr float kGalleryMatShade = 0.15f;
inline constexpr std::array<std::uint8_t, 3> kGalleryLightboxMat{12, 12, 14};

/// The picture of a frame of `kind` from a medium's file `bytes` (PNG or JPEG, its EXIF orientation applied, its alpha
/// composited over the mat): RGBA8 sRGB, opaque, galleryUploadSize(kind). Errors: those of the decoding (with
/// `context`), of the resampling.
[[nodiscard]] Expected<assets::ImageData> makeGalleryPicture(std::span<const std::byte> bytes, world::GalleryKind kind,
                                                             std::string_view context);

} // namespace rv::app
