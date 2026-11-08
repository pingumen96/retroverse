#pragma once

// A console model of the user's content (I11 step 7, ADR-0019 §7), GPU-free: `<user data>/content/consoles/<system>/`
// holds `console.glb` and `console.json`, which retroverse-cli content add-console writes and nothing else does (the
// content root is read-only to everything but that command). Read here: the manifest, the model's checks (its size,
// its triangles, its textures' memory once compressed), its textures compressed to S3TC blocks for the GPU, and the
// socket the insertion takes. The app shows a model that passes in place of the procedural console of its system's
// room (WorldConsoles); one that fails is never shown, its room keeping the procedural console.
//
// console.json (schema 1; unknown keys refused):
//   {"schemaVersion": 1, "model": "console.glb", "kind": "home" | "handheld" | "computer",
//    "front": "+z" | "-z" | "+x" | "-x"  (the axis the console's front faces in the model; +z when absent),
//    "sockets": {"CartridgeSlot": "<node>", "DiscTray": "<node>", "Front": "<node>"}  (optional: else the model's
//               Anchor_CartridgeSlot, Anchor_DiscTray, Anchor_Front),
//    "scaleCm": [width, height, depth]  (optional: the real console's size, checked against the model's),
//    "source": {"url": "...", "author": "...", "license": "...", "date": "YYYY-MM-DD", "modifications": "..."}}
// A model is for personal use: its licence only has to allow that (ADR-0019 §1); the manifest records where it came
// from, never a copy of the licence.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <rv/assets/model_data.hpp>
#include <rv/core/expected.hpp>

namespace rv::assets {

inline constexpr std::string_view kConsolesFolder = "consoles";
inline constexpr std::string_view kConsoleManifestName = "console.json";
inline constexpr std::string_view kConsoleModelName = "console.glb";
inline constexpr int kConsoleManifestSchema = 1;

/// The checks (ADR-0019 §7): each side 5 to 45 cm; the footprint (width x depth, the front turned to +Z) within the hero
/// corner's free area beside the CRT; at most 60,000 triangles drawn; at most 2 MiB of GPU textures once compressed,
/// their mips included.
inline constexpr float kConsoleMinSide = 0.05F;
inline constexpr float kConsoleMaxSide = 0.45F;
inline constexpr float kConsoleMaxWidth = 0.425F;
inline constexpr float kConsoleMaxDepth = 0.40F;
inline constexpr std::size_t kConsoleMaxTriangles = 60000;
inline constexpr std::size_t kConsoleMaxTextureBytes = 2u * 1024u * 1024u;
/// scaleCm against the model: each side within this share of the real one.
inline constexpr float kConsoleScaleTolerance = 0.10F;

enum class ConsoleKind : std::uint8_t { Home, Handheld, Computer };
enum class ConsoleFront : std::uint8_t { PlusZ, MinusZ, PlusX, MinusX };

[[nodiscard]] std::string_view toString(ConsoleKind kind) noexcept;
[[nodiscard]] std::string_view toString(ConsoleFront front) noexcept;
[[nodiscard]] std::optional<ConsoleKind> consoleKindFromString(std::string_view text) noexcept;
[[nodiscard]] std::optional<ConsoleFront> consoleFrontFromString(std::string_view text) noexcept;

/// Where the model came from.
struct ConsoleSourceInfo {
    std::string url;
    std::string author;
    std::string license;
    std::string date;
    std::string modifications;

    bool operator==(const ConsoleSourceInfo&) const = default;
};

struct ConsoleManifest {
    /// The model's file, in the console's folder (a plain file name: no folder, no "..").
    std::string model = std::string(kConsoleModelName);
    ConsoleKind kind = ConsoleKind::Home;
    ConsoleFront front = ConsoleFront::PlusZ;
    /// A socket's role ("CartridgeSlot", "DiscTray", "Front") to the glTF node that marks it.
    std::map<std::string, std::string> sockets;
    /// The real console's width, height and depth in centimetres.
    std::optional<glm::vec3> scaleCm;
    ConsoleSourceInfo source;

    bool operator==(const ConsoleManifest&) const = default;
};

/// Errors: ParseError (not JSON with comments, or not an object), ValidationFailed (a key or value outside the schema),
/// each naming its place in `context`.
[[nodiscard]] Expected<ConsoleManifest> parseConsoleManifest(std::string_view text, std::string_view context);
/// The manifest as JSON text, every key written, as parseConsoleManifest() reads it back.
[[nodiscard]] std::string writeConsoleManifest(const ConsoleManifest& manifest);

/// `<contentDir>/consoles/<system>`.
[[nodiscard]] std::filesystem::path consoleFolder(const std::filesystem::path& contentDir, std::string_view system);

/// The rotation about +Y that turns the model's front (`front`) to +Z.
[[nodiscard]] glm::mat4 consoleFrontRotation(ConsoleFront front) noexcept;
/// From the model's frame to the console's: its front turned to +Z, the middle of its bounds' bottom at the origin
/// (where the procedural console stands on the cabinet).
[[nodiscard]] glm::mat4 consolePlacement(const ModelData& model, ConsoleFront front) noexcept;

/// S3TC blocks of a texture for the GPU: base colour and emissive sRGB, the other maps linear; BC3 where a base colour
/// has transparency, else BC1.
enum class TextureBlocks : std::uint8_t { Bc1Srgb, Bc1Linear, Bc3Srgb, Bc3Linear };
struct CompressedLevel {
    int width = 0;
    int height = 0;
    std::vector<std::byte> blocks;
};
struct CompressedTexture {
    /// The model's image (ModelData::images) and the colour space it is sampled in.
    std::uint32_t image = 0;
    bool srgb = true;
    TextureBlocks format = TextureBlocks::Bc1Srgb;
    /// Its full mip chain.
    std::vector<CompressedLevel> levels;

    [[nodiscard]] std::size_t bytes() const noexcept;
};

/// The model's images its materials sample, by colour space: (image, sRGB) pairs, each once, in material order.
[[nodiscard]] std::vector<std::pair<std::uint32_t, bool>> sampledImages(const ModelData& model);
/// What the sampled images take on the GPU once compressed (sampledImages(), their full mip chains), without compressing.
[[nodiscard]] std::size_t compressedTextureBytes(const ModelData& model);
/// Each sampled image compressed with its mip chain (sRGB levels filtered in linear light, linear maps as they are).
[[nodiscard]] std::vector<CompressedTexture> compressConsoleTextures(const ModelData& model);

/// What the checks found: the console's size once placed (width x, height y, depth z, metres), the triangles drawn, its
/// textures' compressed bytes, and each problem in a sentence (none: it may be shown).
struct ConsoleCheck {
    glm::vec3 size{0.0F};
    std::size_t triangles = 0;
    std::size_t textureBytes = 0;
    std::vector<std::string> problems;

    [[nodiscard]] bool ok() const noexcept { return problems.empty(); }
};
[[nodiscard]] ConsoleCheck checkConsoleModel(const ModelData& model, const ConsoleManifest& manifest);

/// The socket the insertion takes (ADR-0016 §11), in the console's frame (consolePlacement()): "CartridgeSlot", else
/// "DiscTray" (the manifest's nodes first, else the model's anchors), else the top of its bounds' middle; its opening's
/// width and depth (0 when the socket has no extent: the item keeps its size).
struct ConsoleMediaSocket {
    glm::mat4 transform{1.0F};
    glm::vec2 opening{0.0F};
    /// "CartridgeSlot", "DiscTray" or "top".
    std::string from;
};
[[nodiscard]] ConsoleMediaSocket consoleMediaSocket(const ModelData& model, const ConsoleManifest& manifest);

/// A console's folder read: its manifest and its model (images decoded).
struct ConsoleContent {
    std::filesystem::path folder;
    ConsoleManifest manifest;
    ModelData model;
};
/// Reads `folder`'s console.json and the model it names. Errors: NotFound (no console.json), those of
/// parseConsoleManifest() and of loadGltf(), ValidationFailed (a model name that leaves the folder).
[[nodiscard]] Expected<ConsoleContent> loadConsoleContent(const std::filesystem::path& folder);

} // namespace rv::assets
