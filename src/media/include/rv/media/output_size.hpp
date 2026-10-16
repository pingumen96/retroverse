#pragma once

#include <rv/media/video_frame.hpp>

namespace rv::media {

/// Size of the frames a decoder delivers for a stream of `coded` pixels whose pixels are `sampleAspect` times wider
/// than tall (the stream's sample aspect ratio; 0, negative or non-finite means square pixels), at most `maxHeight`
/// rows tall (0 or less: no limit). Specification §9.5 (previews of 480p or 720p), style §15.2 (aspect ratio kept):
///  - the display aspect ratio, coded width * sampleAspect / coded height, is kept; non-square pixels are corrected by
///    shrinking the side they stretch (width for sampleAspect < 1, height for sampleAspect > 1), never by enlarging the
///    other one;
///  - the picture is then scaled down to fit maxHeight, never up;
///  - sides are rounded to the nearest pixel and are at least 1.
/// An invalid `coded` size gives an empty size.
[[nodiscard]] FrameSize scaledFrameSize(FrameSize coded, double sampleAspect, int maxHeight) noexcept;

} // namespace rv::media
