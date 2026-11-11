#include <rv/app/gallery_pictures.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <utility>

#include <rv/assets/cover_fit.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/assets/mip_chain.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/seed.hpp>
#include <rv/world/room_selection.hpp>

namespace rv::app {
namespace {

/// `relative` under the media root, as the catalog names it; nothing when it is empty or makes no path.
[[nodiscard]] std::filesystem::path below(const std::filesystem::path& root, const std::string& relative) {
    if (relative.empty()) {
        return {};
    }
    auto path = core::extendedLengthPathBelow(root, relative);
    return path ? std::move(*path) : std::filesystem::path();
}

/// The first of `paths` that is not empty.
[[nodiscard]] std::filesystem::path firstOf(std::initializer_list<std::filesystem::path> paths) {
    for (const std::filesystem::path& path : paths) {
        if (!path.empty()) {
            return path;
        }
    }
    return {};
}

[[nodiscard]] std::size_t indexOf(world::GalleryKind kind) noexcept {
    return static_cast<std::size_t>(kind);
}

/// sRGB bytes to linear light, and back.
[[nodiscard]] float toLinear(std::uint8_t value) noexcept {
    const float c = static_cast<float>(value) / 255.0F;
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}
[[nodiscard]] std::uint8_t toSrgb(float linear) noexcept {
    const float c = std::clamp(linear, 0.0F, 1.0F);
    const float encoded = c <= 0.0031308F ? c * 12.92F : 1.055F * std::pow(c, 1.0F / 2.4F) - 0.055F;
    return static_cast<std::uint8_t>(std::lround(std::clamp(encoded, 0.0F, 1.0F) * 255.0F));
}

} // namespace

RoomGallery roomGallery(const world::CollectionLayout& layout, const GalleryItemLookup& itemOf,
                        const std::filesystem::path& mediaRoot, std::uint64_t seed) {
    struct Ranked {
        world::RoomTitle title;
        GalleryCandidate candidate;
    };
    std::vector<Ranked> ranked;
    std::set<catalog::GameId> seen;
    // The root in its \\?\ form once (extendedLengthPathBelow() takes it as it is), not normalized for every path.
    const std::filesystem::path root(core::extendedLengthPath(mediaRoot));
    for (const world::CollectionItem& item : layout.items) {
        const RoomItemText* text = itemOf ? itemOf(item.game) : nullptr;
        if (text == nullptr || !text->screen || !seen.insert(item.game).second) {
            continue;
        }
        const RoomItemScreen& screen = *text->screen;
        GalleryCandidate candidate;
        candidate.game = item.game;
        candidate.media[indexOf(world::GalleryKind::Poster)] = below(root, screen.fanArt);
        candidate.media[indexOf(world::GalleryKind::Portrait)] =
            firstOf({below(root, screen.box3d), below(root, text->coverPath)});
        candidate.media[indexOf(world::GalleryKind::Lightbox)] = below(root, screen.marquee);
        candidate.media[indexOf(world::GalleryKind::Frame)] =
            firstOf({below(root, screen.screenshot), below(root, screen.titleScreen), below(root, screen.mixImage)});
        if (std::all_of(candidate.media.begin(), candidate.media.end(),
                        [](const std::filesystem::path& path) { return path.empty(); })) {
            continue;
        }
        Ranked entry;
        entry.title.game = item.game;
        entry.title.sortKey = screen.sortKey;
        entry.title.favorite = screen.favorite;
        entry.title.playCount = screen.playCount;
        entry.title.ratingTenths = screen.ratingTenths;
        entry.candidate = std::move(candidate);
        ranked.push_back(std::move(entry));
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const Ranked& a, const Ranked& b) { return world::priorityLess(a.title, b.title); });
    RoomGallery gallery;
    gallery.seed = seed;
    gallery.candidates.reserve(ranked.size());
    for (Ranked& entry : ranked) {
        gallery.candidates.push_back(std::move(entry.candidate));
    }
    return gallery;
}

std::vector<GalleryTarget> galleryTargets(const scene::SceneDescription& description) {
    std::vector<GalleryTarget> targets;
    const double bars = 2.0 * static_cast<double>(world::kGalleryFrameBarMm) / 1000.0;
    for (const scene::SceneObject& object : description.objects) {
        if (object.source != scene::SceneObject::Source::Prefab || object.kind != "poster_frame") {
            continue;
        }
        const std::size_t at = object.id.find(world::kGalleryObjectPrefix);
        if (at == std::string::npos || (at > 0 && object.id[at - 1] != '/')) {
            continue;
        }
        GalleryTarget target;
        target.objectId = object.id;
        target.room = at > 0 ? object.id.substr(0, at - 1) : std::string();
        target.fixture = object.id.substr(at + world::kGalleryObjectPrefix.size());
        const double width = object.params.value("width", 0.0) - bars;
        const double height = object.params.value("height", 0.0) - bars;
        if (target.fixture.empty() || width <= 0.0 || height <= 0.0) {
            continue;
        }
        target.kind = world::galleryKindOfAspect(width / height);
        targets.push_back(std::move(target));
    }
    return targets;
}

std::vector<GalleryChoice> chooseGalleryPictures(std::span<const GalleryTarget> targets, const RoomGallery& gallery) {
    std::vector<GalleryChoice> choices;
    const std::size_t count = gallery.candidates.size();
    const std::size_t pool = std::min(count, 2 * targets.size());
    std::vector<bool> used(count, false);
    for (const GalleryTarget& target : targets) {
        const std::size_t kind = indexOf(target.kind);
        std::vector<std::size_t> eligible;
        for (const std::size_t limit : {pool, count}) {
            for (std::size_t index = 0; index < limit; ++index) {
                if (!used[index] && !gallery.candidates[index].media[kind].empty()) {
                    eligible.push_back(index);
                }
            }
            if (!eligible.empty()) {
                break;
            }
        }
        if (eligible.empty()) {
            continue; // the print stays
        }
        const std::uint64_t draw = core::deriveSeed(gallery.seed, "gallery-game", target.fixture);
        const std::size_t pick = eligible[static_cast<std::size_t>(draw % eligible.size())];
        used[pick] = true;
        const GalleryCandidate& candidate = gallery.candidates[pick];
        choices.push_back(GalleryChoice{target.objectId, candidate.game, candidate.media[kind], target.kind});
    }
    return choices;
}

glm::ivec2 galleryUploadSize(world::GalleryKind kind) noexcept {
    switch (kind) {
    case world::GalleryKind::Poster:
        return {1024, 576};
    case world::GalleryKind::Portrait:
        return {640, 800};
    case world::GalleryKind::Lightbox:
        return {1024, 344};
    case world::GalleryKind::Frame:
        return {768, 576};
    }
    return {1024, 576};
}

Expected<assets::ImageData> makeGalleryPicture(std::span<const std::byte> bytes, world::GalleryKind kind,
                                               std::string_view context) {
    auto decoded = assets::decodeUprightImage(bytes, 4, context);
    if (!decoded) {
        return propagate(decoded.error());
    }
    assets::ImageData& source = *decoded;
    const glm::ivec2 size = galleryUploadSize(kind);
    // The mat: the dominant colour of the medium's opaque pixels (a 3D box's or a marquee's transparent ground left
    // out) darkened in linear light; a lightbox's nearly black.
    assets::ImageData opaque;
    opaque.channels = 4;
    for (std::size_t pixel = 0; pixel < source.pixels.size(); pixel += 4) {
        if (source.pixels[pixel + 3] >= 128) {
            opaque.pixels.insert(opaque.pixels.end(), source.pixels.begin() + static_cast<std::ptrdiff_t>(pixel),
                                 source.pixels.begin() + static_cast<std::ptrdiff_t>(pixel + 4));
        }
    }
    opaque.width = static_cast<int>(opaque.pixels.size() / 4);
    opaque.height = 1;
    const assets::Rgb8 dominant = assets::dominantColour(opaque.width > 0 ? opaque : source);
    const std::array<std::uint8_t, 3> mat =
        kind == world::GalleryKind::Lightbox
            ? kGalleryLightboxMat
            : std::array<std::uint8_t, 3>{toSrgb(toLinear(dominant.r) * kGalleryMatShade),
                                          toSrgb(toLinear(dominant.g) * kGalleryMatShade),
                                          toSrgb(toLinear(dominant.b) * kGalleryMatShade)};
    // Its transparency composited over the mat (a marquee's lettering on it), in linear light.
    std::array<float, 256> linear{};
    for (int value = 0; value < 256; ++value) {
        linear[static_cast<std::size_t>(value)] = toLinear(static_cast<std::uint8_t>(value));
    }
    for (std::size_t pixel = 0; pixel < source.pixels.size(); pixel += 4) {
        const std::uint8_t alpha = source.pixels[pixel + 3];
        if (alpha == 255) {
            continue;
        }
        const float a = static_cast<float>(alpha) / 255.0F;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float over = linear[source.pixels[pixel + channel]] * a + linear[mat[channel]] * (1.0F - a);
            source.pixels[pixel + channel] = toSrgb(over);
        }
        source.pixels[pixel + 3] = 255;
    }
    // Fitted inside the picture (a frame's within its mat), never stretched.
    const float inset = kind == world::GalleryKind::Frame ? kGalleryFrameMat : 0.0F;
    const float innerWidth = static_cast<float>(size.x) * (1.0F - 2.0F * inset);
    const float innerHeight = static_cast<float>(size.y) * (1.0F - 2.0F * inset);
    const float scale =
        std::min(innerWidth / static_cast<float>(source.width), innerHeight / static_cast<float>(source.height));
    const int width = std::clamp(static_cast<int>(std::lround(static_cast<float>(source.width) * scale)), 1, size.x);
    const int height = std::clamp(static_cast<int>(std::lround(static_cast<float>(source.height) * scale)), 1, size.y);
    auto fitted = assets::resampleSrgb(source, width, height);
    if (!fitted) {
        return propagate(fitted.error());
    }
    assets::ImageData picture;
    picture.width = size.x;
    picture.height = size.y;
    picture.channels = 4;
    picture.pixels.resize(static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) * 4);
    for (std::size_t pixel = 0; pixel < picture.pixels.size(); pixel += 4) {
        picture.pixels[pixel] = mat[0];
        picture.pixels[pixel + 1] = mat[1];
        picture.pixels[pixel + 2] = mat[2];
        picture.pixels[pixel + 3] = 255;
    }
    const int left = (size.x - width) / 2;
    const int top = (size.y - height) / 2;
    for (int y = 0; y < height; ++y) {
        const auto from = fitted->pixels.begin() + static_cast<std::ptrdiff_t>(y) * width * 4;
        std::copy(from, from + static_cast<std::ptrdiff_t>(width) * 4,
                  picture.pixels.begin() + (static_cast<std::ptrdiff_t>(top + y) * size.x + left) * 4);
    }
    return picture;
}

} // namespace rv::app
