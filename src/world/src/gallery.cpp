#include <rv/world/gallery.hpp>

#include <cmath>
#include <limits>

namespace rv::world {

std::string_view toString(GalleryKind kind) noexcept {
    switch (kind) {
    case GalleryKind::Poster:
        return "poster";
    case GalleryKind::Portrait:
        return "portrait";
    case GalleryKind::Lightbox:
        return "lightbox";
    case GalleryKind::Frame:
        return "frame";
    }
    return "poster";
}

std::optional<GalleryKind> parseGalleryKind(std::string_view text) noexcept {
    for (const GalleryKind kind : kGalleryKinds) {
        if (toString(kind) == text) {
            return kind;
        }
    }
    return std::nullopt;
}

GalleryPictureSize galleryPictureSize(GalleryKind kind) noexcept {
    switch (kind) {
    case GalleryKind::Poster:
        return {704, 396};
    case GalleryKind::Portrait:
        return {480, 600};
    case GalleryKind::Lightbox:
        return {780, 260};
    case GalleryKind::Frame:
        return {600, 450};
    }
    return {704, 396};
}

GalleryKind galleryKindOfAspect(double aspect) noexcept {
    GalleryKind nearest = GalleryKind::Poster;
    double best = std::numeric_limits<double>::max();
    for (const GalleryKind kind : kGalleryKinds) {
        const GalleryPictureSize size = galleryPictureSize(kind);
        const double own = static_cast<double>(size.widthMm) / static_cast<double>(size.heightMm);
        const double distance = aspect > 0.0 ? std::abs(std::log(aspect / own)) : std::numeric_limits<double>::max();
        if (distance < best) {
            best = distance;
            nearest = kind;
        }
    }
    return nearest;
}

} // namespace rv::world
