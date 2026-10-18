#pragma once

// GPU-free (target rv_render_uniforms): no OpenGL header is included.
//
// The collection's texture arrays as data (ADR-0011 §4, §6; I3 step 6): the two layer classes and their sizes, the
// storage format and its fallback, how many layers an array of a room gets and how a class splits into arrays of at
// most 2,048 layers, the layer images the producers hand over (one mip chain per layer), the layers of one array with
// their owners and the least recently released ones that may be reused, and the per-frame upload budget. The GL side
// (gl::Texture2DArray, gl::CollectionArrays) follows these rules exactly; tests/unit/render checks them without a
// context.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec2.hpp>

#include <rv/core/expected.hpp>
#include <rv/render/gpu_memory.hpp>

namespace rv::gl {

/// What a layer shows: the front label (the cover, or the generated label) or the spine (the generated title).
enum class CollectionClass : std::uint8_t { Front, Spine };
inline constexpr std::size_t kCollectionClassCount = 2;
inline constexpr std::array<CollectionClass, kCollectionClassCount> kCollectionClasses{CollectionClass::Front,
                                                                                       CollectionClass::Spine};

/// "front" or "spine".
[[nodiscard]] std::string_view toString(CollectionClass cls) noexcept;
[[nodiscard]] constexpr std::size_t toIndex(CollectionClass cls) noexcept {
    return static_cast<std::size_t>(cls);
}

/// Storage of the collection's arrays. Both are sRGB: sampling returns linear values.
enum class CollectionArrayFormat : std::uint8_t {
    /// GL_COMPRESSED_SRGB_S3TC_DXT1_EXT at the classes' full size (opaque BC1: 8 bytes per 4 x 4 block).
    Bc1Srgb,
    /// GL_SRGB8_ALPHA8 at half the classes' size: without GL_EXT_texture_compression_s3tc and GL_EXT_texture_sRGB, or
    /// with --no-texture-compression. Twice the memory of BC1 at full size.
    Rgba8Srgb,
};

/// "bc1-srgb" or "rgba8-srgb".
[[nodiscard]] std::string_view toString(CollectionArrayFormat format) noexcept;
/// The format the GPU memory accounting computes the format's bytes with: GpuFormat::Bc1 or GpuFormat::Srgb8Alpha8.
[[nodiscard]] GpuFormat gpuFormatOf(CollectionArrayFormat format) noexcept;

/// The format of a run and why: BC1 sRGB when both S3TC extensions are present (GLCapabilities::s3tcSrgb()) and
/// compression is not turned off, RGBA8 otherwise.
struct CollectionFormatChoice {
    CollectionArrayFormat format = CollectionArrayFormat::Bc1Srgb;
    /// What decided it, for the log: "GL_EXT_texture_compression_s3tc and GL_EXT_texture_sRGB present",
    /// "--no-texture-compression" or "GL_EXT_texture_compression_s3tc or GL_EXT_texture_sRGB missing".
    std::string_view reason;
};

/// The run's choice from the device's extensions and --no-texture-compression (which wins when both apply).
[[nodiscard]] CollectionFormatChoice chooseCollectionArrayFormat(bool s3tcSrgbAvailable,
                                                                 bool compressionDisabled) noexcept;

/// Full-size layers: the front at the covers' 1.368 aspect, the spine at the 2.0 x 12.5 cm spine's.
inline constexpr glm::ivec2 kFrontLayerSize{256, 188};
inline constexpr glm::ivec2 kSpineLayerSize{40, 256};

/// A layer of `cls` in `format`: the full size for BC1, half of it for RGBA8 (128 x 94 and 20 x 128).
[[nodiscard]] glm::ivec2 layerSize(CollectionClass cls, CollectionArrayFormat format) noexcept;
/// Mip levels of a layer of `cls` in `format`: the full chain down to 1 x 1 (9 at full size, 8 at half).
[[nodiscard]] int layerLevelCount(CollectionClass cls, CollectionArrayFormat format) noexcept;
/// Size of mip `level` of a texture of `base`: each side halved per level, at least 1. Empty for a negative level.
[[nodiscard]] glm::ivec2 mipLevelSize(glm::ivec2 base, int level) noexcept;
/// Bytes of one level of a layer of `cls` in `format` (BC1: whole 4 x 4 blocks, every side rounded up).
[[nodiscard]] std::size_t layerLevelBytes(CollectionClass cls, CollectionArrayFormat format, int level) noexcept;
/// Bytes of one layer of `cls` in `format`, every level included: what each layer of an array takes on the GPU.
[[nodiscard]] std::size_t layerBytes(CollectionClass cls, CollectionArrayFormat format) noexcept;
/// Bytes of an array of `cls` in `format` with `layers` layers (textureBytes of its size, levels and layers).
[[nodiscard]] std::size_t arrayBytes(CollectionClass cls, CollectionArrayFormat format, int layers) noexcept;

/// Layers of one array: OpenGL 4.5's guaranteed GL_MAX_ARRAY_TEXTURE_LAYERS.
inline constexpr int kMaxArrayLayers = 2048;
/// Arrays are created and grown in multiples of this many layers.
inline constexpr int kArrayLayerGranularity = 64;
/// The layer every array reserves for the pending picture: an item whose layer has not arrived shows it (its instance
/// record holds 0, the default).
inline constexpr std::uint32_t kPendingLayer = 0;
/// Items one array holds: all its layers but the pending one.
inline constexpr int kMaxItemsPerArray = kMaxArrayLayers - 1;

/// Layers of an array that holds `items` items: the items plus the pending layer, rounded up to a multiple of
/// kArrayLayerGranularity, at most kMaxArrayLayers (an array of no item still has its 64).
[[nodiscard]] int arrayLayersFor(std::size_t items) noexcept;
/// The arrays of one class for `items` items (the room's placements and free slots), as their layer counts: as many
/// full arrays of kMaxArrayLayers as kMaxItemsPerArray items fill, then one sized by arrayLayersFor for the rest (none
/// when nothing is left; a single 64-layer array for no item). 2,049 items: {2048, 64}.
[[nodiscard]] std::vector<int> planArrayLayers(std::size_t items);

/// An sRGB color of 8 bits per channel.
struct Rgb8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    bool operator==(const Rgb8&) const = default;
};

/// 5:6:5 bits of `color` (each channel rounded to the nearest representable value), BC1's endpoint form.
[[nodiscard]] std::uint16_t toRgb565(Rgb8 color) noexcept;
/// The 8-bit color a BC1 endpoint of `rgb565` decodes to, by bit replication ((r5 << 3) | (r5 >> 2), ...).
[[nodiscard]] Rgb8 fromRgb565(std::uint16_t rgb565) noexcept;

/// One layer's picture with its mip chain, as the cover and label pipeline produces it (ADR-0011 §5 and §7: the levels
/// of an .rvtex entry): every level of layerLevelCount(cls, format), level 0 first, each exactly layerLevelBytes long
/// (BC1 levels whose sides are not multiples of 4 as whole blocks, their edge pixels repeated; RGBA8 tightly packed,
/// rows top first, alpha ignored by the shaders).
struct LayerImage {
    CollectionArrayFormat format = CollectionArrayFormat::Bc1Srgb;
    std::vector<std::vector<std::byte>> levels;
};

/// Ok when `image` is a layer of `cls` in `format`: the same format, the class's level count, every level of its exact
/// size. InvalidArgument naming the first difference otherwise.
[[nodiscard]] Status validateLayerImage(const LayerImage& image, CollectionClass cls, CollectionArrayFormat format);

/// A layer of `cls` in `format` filled with `color` at every level (the pending picture, test images): BC1 blocks with
/// both endpoints toRgb565(color) and every index 0, so that it decodes to fromRgb565 of it exactly; RGBA8 texels of
/// the color itself, opaque.
[[nodiscard]] LayerImage makeSolidLayer(CollectionClass cls, CollectionArrayFormat format, Rgb8 color);

/// The layers of one array: who owns each (a key the caller chooses, such as the item's game id and class), whether its
/// picture is uploaded (resident), and the layers released by an item that left the room, kept with their pictures in
/// case it comes back and reused, least recently released first, only when no layer was ever used is left. Layer 0 is
/// the pending picture and never given out. Each assignment of a layer to a key bumps its generation, so that an
/// upload queued for an earlier owner is recognized and dropped. Deterministic (ordered containers only).
class LayerAllocator {
public:
    /// What acquire() gave.
    struct Acquired {
        std::uint32_t layer = kPendingLayer;
        /// The generation of the layer's current assignment, for markResident().
        std::uint32_t generation = 0;
        /// The layer holds this key's picture already (it kept it while released): no upload is needed.
        bool resident = false;
        /// The key held the layer before (in use or released) rather than getting a new one.
        bool reused = false;
    };

    /// An allocator of `layers` layers (at least 1: layer 0), none given out.
    explicit LayerAllocator(int layers = kArrayLayerGranularity);

    /// The layer of `key`: its own (in use, or released and taken back with its picture), else a layer never used,
    /// else the least recently released one (its earlier key forgotten), else nothing (every layer is in use).
    [[nodiscard]] std::optional<Acquired> acquire(std::string_view key);
    /// Gives back the layer of `key`, which keeps its picture until it is reused. False for a key without a layer in
    /// use.
    bool release(std::string_view key);
    /// Records that `layer`'s picture arrived for the assignment of `generation`; false (nothing changes) for a layer
    /// out of range, unassigned, or assigned again since.
    bool markResident(std::uint32_t layer, std::uint32_t generation);
    /// Adds layers up to `layers` in total (an array grown with its pictures copied); fewer than now changes nothing.
    void grow(int layers);

    /// The layer `key` holds (in use or released), if any.
    [[nodiscard]] std::optional<std::uint32_t> layerOf(std::string_view key) const;
    /// The generation of `layer`'s current assignment (0 for one never assigned).
    [[nodiscard]] std::uint32_t generation(std::uint32_t layer) const noexcept;
    [[nodiscard]] bool resident(std::uint32_t layer) const noexcept;
    [[nodiscard]] bool inUse(std::uint32_t layer) const noexcept;

    /// Layers of the array, layer 0 included.
    [[nodiscard]] int capacity() const noexcept { return static_cast<int>(m_layers.size()); }
    /// Layers held by a key in use.
    [[nodiscard]] std::size_t usedCount() const noexcept { return m_used; }
    /// Layers released and not reused yet.
    [[nodiscard]] std::size_t releasedCount() const noexcept { return m_released; }
    /// Layers never given out (layer 0 excluded).
    [[nodiscard]] std::size_t neverUsedCount() const noexcept;
    /// Layers in use whose picture arrived.
    [[nodiscard]] std::size_t residentUsedCount() const noexcept;

private:
    enum class State : std::uint8_t { Free, InUse, Released };
    struct Layer {
        State state = State::Free;
        bool resident = false;
        std::uint32_t generation = 0;
        /// When it was released (m_clock), for the least recently released.
        std::uint64_t releasedAt = 0;
        std::string key;
    };

    std::uint32_t assign(std::uint32_t layer, std::string_view key);

    std::vector<Layer> m_layers;
    std::map<std::string, std::uint32_t, std::less<>> m_keys;
    /// The next layer never given out.
    std::uint32_t m_nextFree = 1;
    std::size_t m_used = 0;
    std::size_t m_released = 0;
    std::uint64_t m_clock = 0;
};

/// How much of a frame's main thread the collection's uploads may take (ADR-0011 §6: 1 ms or 64 layers, whichever comes
/// first).
struct UploadBudget {
    std::chrono::nanoseconds time{std::chrono::milliseconds(1)};
    std::size_t layers = 64;
};

/// Decides, layer by layer, whether the next upload of a frame fits its budget: always the first (so that uploads
/// progress whatever their cost), then while fewer than UploadBudget::layers went up and the time taken plus the
/// expected cost of one more layer stays within UploadBudget::time. The expected cost is a moving average of the
/// measured layers (a quarter of each new measurement), kept from frame to frame.
class UploadBudgetMeter {
public:
    /// Starts a frame under `budget` (the average carries over).
    void beginFrame(const UploadBudget& budget) noexcept;
    /// Whether one more layer may go up, `elapsed` after the frame's uploads began.
    [[nodiscard]] bool mayUploadNext(std::chrono::nanoseconds elapsed) const noexcept;
    /// Records one layer uploaded in `cost`.
    void recordLayer(std::chrono::nanoseconds cost) noexcept;

    [[nodiscard]] std::size_t layersThisFrame() const noexcept { return m_layers; }
    /// The expected cost of a layer (0 before the first measurement).
    [[nodiscard]] std::chrono::nanoseconds expectedLayerCost() const noexcept { return m_average; }

private:
    UploadBudget m_budget;
    std::size_t m_layers = 0;
    std::chrono::nanoseconds m_average{0};
};

} // namespace rv::gl
