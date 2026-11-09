#pragma once

// The gallery's frames (I12 step 2, ADR-0021 §2): framed pictures over a room's modules, which the scene builder hangs
// with the pack's print in them (room_look.hpp's "roomDressing" "gallery") and the app fills with the user's own media
// of the room's games (app/gallery_pictures.hpp). GPU-free.
//
// A frame's kind is the shape of its picture: a poster 16:9 (fanart), a portrait 4:5 (a 3D box, else a cover), a
// lightbox 3:1 (a marquee, lit) and a frame 4:3 (a screenshot, a title screen or a miximage on a mat). The scene file
// carries only the frame's size: the app reads the kind back from its picture's aspect (galleryKindOfAspect()), so the
// scene format needs nothing new.

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace rv::world {

enum class GalleryKind : std::uint8_t { Poster, Portrait, Lightbox, Frame };

inline constexpr std::array<GalleryKind, 4> kGalleryKinds{GalleryKind::Poster, GalleryKind::Portrait,
                                                          GalleryKind::Lightbox, GalleryKind::Frame};

/// The frames' scene objects: "dressing:gallery:<fixture's local id>" (a poster_frame), its picture light's hood
/// "<id>/hood" and wash "<id>/wash".
inline constexpr std::string_view kGalleryObjectPrefix = "dressing:gallery:";

/// The frames' bars, all kinds: this wide and deep.
inline constexpr std::int64_t kGalleryFrameBarMm = 25;
inline constexpr std::int64_t kGalleryFrameDepthMm = 30;

/// A kind's picture (inside the bars), millimetres.
struct GalleryPictureSize {
    std::int64_t widthMm = 0;
    std::int64_t heightMm = 0;
};

/// "poster", "portrait", "lightbox", "frame".
[[nodiscard]] std::string_view toString(GalleryKind kind) noexcept;
[[nodiscard]] std::optional<GalleryKind> parseGalleryKind(std::string_view text) noexcept;

/// The picture of a frame of `kind`: poster 704 x 396, portrait 480 x 600, lightbox 780 x 260, frame 600 x 450 (each
/// narrower than a module's 0.9 m with its bars, so neighbouring frames stand apart; even heights, so a frame's middle
/// is a whole millimetre from its edges).
[[nodiscard]] GalleryPictureSize galleryPictureSize(GalleryKind kind) noexcept;

/// The kind whose picture's aspect (width / height) is nearest `aspect` in ratio.
[[nodiscard]] GalleryKind galleryKindOfAspect(double aspect) noexcept;

} // namespace rv::world
