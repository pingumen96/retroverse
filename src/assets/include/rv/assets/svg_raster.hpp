#pragma once

// Logos as pictures (I11 step 2, ADR-0019 §3): an SVG rasterized with lunasvg, or a PNG or JPEG decoded, fitted into a
// box at its own aspect, as RGBA8 sRGB with straight (not premultiplied) alpha. The files are the user's (ES-DE's
// themes, identity.json): lunasvg only sees a document that passed the bounds below, which refuse what could reach
// past the document (an href to a file or an address, a CSS @import) or blow up as it is read (entity declarations).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <rv/assets/image_data.hpp>
#include <rv/core/expected.hpp>

namespace rv::assets {

/// The largest SVG document rasterized.
inline constexpr std::size_t kMaxSvgBytes = 1024u * 1024u;
/// The most elements ('<' that open a tag) an SVG document may hold.
inline constexpr std::size_t kMaxSvgElements = 50000;
/// The largest side a logo is rasterized at.
inline constexpr int kMaxLogoSide = 4096;

/// The pixel size a picture of `width` x `height` takes when fitted into `boxWidth` x `boxHeight` at its own aspect: the
/// larger that fits, each side at least 1. Nothing when a size is not positive.
[[nodiscard]] std::optional<std::pair<int, int>> fitInto(double width, double height, int boxWidth, int boxHeight);

/// Rasterizes an SVG document held in memory, fitted into `boxWidth` x `boxHeight` (each 1 to kMaxLogoSide) at the
/// aspect of its intrinsic size (width and height, else its viewBox), transparent where it draws nothing. `context`
/// names the source in messages. Errors: InvalidArgument (a box outside 1 to kMaxLogoSide, an empty document),
/// OutOfRange (more than kMaxSvgBytes or kMaxSvgElements), Unsupported (a DOCTYPE or an entity declaration, a CSS
/// @import, an href to anything but the document itself ("#id") or a data: URI: a logo needs none of them),
/// ParseError (lunasvg cannot read it, or it has no size).
[[nodiscard]] Expected<ImageData> rasterizeSvg(std::span<const std::byte> document, int boxWidth, int boxHeight,
                                               std::string_view context);

/// A logo file (.svg, .png, .jpg or .jpeg) as RGBA8 sRGB, straight alpha, fitted into `boxWidth` x `boxHeight` at its
/// aspect: an SVG rasterized at that size, a PNG or JPEG decoded and, when larger than the box, resampled down in
/// linear light with its alpha (never up). Read with full sharing. Errors: those of reading (NotFound, OutOfRange past
/// kMaxSvgBytes for an SVG or kMaxImageFileBytes, IoError), of rasterizeSvg() and of decodeImage(), Unsupported for
/// another extension.
[[nodiscard]] Expected<ImageData> loadLogo(const std::filesystem::path& file, int boxWidth, int boxHeight);

} // namespace rv::assets
