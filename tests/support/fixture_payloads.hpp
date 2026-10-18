#pragma once

// The media payloads files.json can generate (I3 step 1c, tests/support/fixture.hpp): covers and other pictures, an
// original procedural picture with the game's title, and short clips from media::test_video, each optionally damaged
// as the media fallbacks need it (style §21, specification SC-08). Deterministic: the same payload always gives the
// same picture bytes; clips come from FFmpeg's own encoders, single-threaded. Nothing commercial: the pictures are
// abstract geometry and the title in the built-in bitmap font (assets::makeCoverPlaceholder).

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include <rv/assets/image_data.hpp>
#include <rv/core/expected.hpp>
#include <rv/media/test_video.hpp>

namespace rv::test {

/// How a generated picture is encoded.
enum class PictureFormat : std::uint8_t {
    Png,
    Jpeg,
};

/// What a generated picture shows.
enum class PictureShape : std::uint8_t {
    /// A box cover: makeCoverPlaceholder's abstract pattern and title band, opaque RGB (as scraped covers are).
    Box,
    /// A cartridge photograph (ES-DE's physicalmedia): a grey cartridge with the cover as its label on a transparent
    /// background, RGBA. PNG only.
    Cartridge,
};

/// How a generated picture is damaged.
enum class PictureDamage : std::uint8_t {
    None,
    /// The encoded file cut to its first half: the format's signature is there, the data ends early (decodeImage:
    /// ParseError).
    Truncated,
    /// A short text in place of the image, under the picture's name (decodeImage: Unsupported).
    NotAnImage,
};

/// A generated picture (files.json: "cover").
struct PicturePayload {
    /// Written on the picture's title band (upper-cased by the bitmap font).
    std::string title;
    /// Colors and layout of the pattern.
    std::uint32_t seed = 0;
    /// Pixels, each 1..4096. The default is a landscape box cover of the size scrapers commonly deliver.
    int width = 680;
    int height = 497;
    PictureFormat format = PictureFormat::Png;
    PictureShape shape = PictureShape::Box;
    PictureDamage damage = PictureDamage::None;
    /// JPEG quality, 1..100.
    int quality = 88;
    /// A PNG through encodeQuickPng() rather than assets::encodePng() (stb_image_write, about 25 ms for a 680 x 497
    /// cover in Release, too slow for the 2,300 covers of a generated collection).
    bool quickPng = false;
};

/// A PNG of `image` (1 to 4 channels, 8 bits) written for speed: each row filtered "Sub" or "Up", whichever leaves more
/// zero bytes, then one deflate block with the fixed Huffman codes whose only matches repeat the previous byte. About
/// 1 ms for a 680 x 497 cover; on the flat procedural pictures a little larger than stb_image_write's. A valid PNG
/// every decoder reads; the same image always gives the same bytes.
[[nodiscard]] std::string encodeQuickPng(const assets::ImageData& image);

/// The undamaged picture: RGB for a box, RGBA for a cartridge.
[[nodiscard]] assets::ImageData pictureImage(const PicturePayload& payload);

/// The file's bytes: the picture encoded, then damaged as asked. Errors: those of the encoders.
[[nodiscard]] Expected<std::string> pictureBytes(const PicturePayload& payload);

/// How a generated clip is damaged (the cases of specification SC-08 that ADR-0004's fallback chain handles).
enum class ClipDamage : std::uint8_t {
    None,
    /// A file of zero bytes (the decoder: InvalidFile).
    Empty,
    /// The clip cut to its first half (an .mp4 keeps its index first, so it opens and ends mid-stream: Truncated).
    Truncated,
    /// Pseudo-random bytes over part of every 16 KiB between 40% and 80% of the clip (media::writeCorruptedCopy).
    Corrupt,
    /// An .avi whose video codec's four-character codes read "RVZZ", which no decoder knows (UnsupportedCodec).
    UnsupportedCodec,
};

/// The clip files.json's "video" gives by default: 4:3, 640 x 480, 30 fps, 4 s, MPEG-4 Part 2 at quantizer 5 (about 0.5
/// MB), key frames every second, no audio.
[[nodiscard]] media::TestVideoSpec defaultClipSpec() noexcept;

/// A generated clip (files.json: "video"): media::writeTestVideo's moving pattern with the frame number burned in
/// (media::decodeFrameCounter reads it back), in the container the file's extension names (.mp4, .mkv, .avi).
struct ClipPayload {
    media::TestVideoSpec spec = defaultClipSpec();
    ClipDamage damage = ClipDamage::None;
    /// The seed of the Corrupt damage's bytes.
    std::uint32_t damageSeed = 0;
};

/// Writes the clip to `target` (a path FFmpeg can open: shorter than MAX_PATH), through a temporary file beside it for
/// the damaged kinds. Errors: InvalidArgument for a target whose extension the payload cannot take (UnsupportedCodec
/// needs .avi), those of media::writeTestVideo and of the copies.
[[nodiscard]] Status writeClip(const std::filesystem::path& target, const ClipPayload& payload);

/// files.json's "cover" object, for a file whose extension is `extension` (".png", ".jpg"): {"title", "seed",
/// "width", "height", "format" ("png" | "jpeg", else the extension's), "shape" ("box" | "cartridge"), "damage"
/// ("truncated" | "not-an-image"), "quality"}; "title" and "seed" are required. Errors: ValidationFailed naming the
/// key, `context` (the file's path in the manifest) prefixed.
[[nodiscard]] Expected<PicturePayload> parsePicturePayload(const nlohmann::json& value, std::string_view extension,
                                                           std::string_view context);

/// files.json's "video" object, for a file whose extension is `extension`: {"seed", "seconds", "width", "height",
/// "frameRate", "codec" ("mpeg4" | "mjpeg"), "quantizer", "damage" ("empty" | "truncated" | "corrupt" |
/// "unsupported-codec")}; "seed" is required. Errors: ValidationFailed naming the key, `context` prefixed.
[[nodiscard]] Expected<ClipPayload> parseClipPayload(const nlohmann::json& value, std::string_view extension,
                                                     std::string_view context);

} // namespace rv::test
