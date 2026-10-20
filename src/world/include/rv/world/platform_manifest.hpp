#pragma once

// The platform visual manifest (ADR-0010 §2, style §22: "the mapping system -> prefab is data, not code"):
// assets/world/platforms/<system>.json, how a system's room looks. Everything in it is original and brand-free (style
// §1, §17.2): the media family its games stand as, the theme pack its decoration comes from, the plastics' colours,
// the console prefab of the hero corner and the sign's text.
//
// The file (JSON with comments; every key required):
//   {"schemaVersion": 1, "system": "snes", "mediaFamily": "cartridge_home_16bit", "themePack": "classic_16bit_warm",
//    "plastic": {"cartridge": "#9A9A96", "console": "#D6CBAE", "accent": "#7D6E9E"},
//    "console": {"manifest": "hardware.console.cartridge.generic", "kind": "console_cartridge"} (or null: none),
//    "sign": {"text": "16-BIT CORNER"}}
// The manifest is optional since I4 (ADR-0013 §2): a system without one takes what systems.json gives it
// (generation.hpp's resolvePlatform()); one with a manifest takes the manifest whole.
// Colours are sRGB hex codes "#RRGGBB" (style §3), uppercase; the room converts them to linear when it draws.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/world/ids.hpp>

namespace rv::world {

/// The sign's text: 1 to 32 printable ASCII characters (U+0020 to U+007E), no space at either end.
inline constexpr std::size_t kMaxSignTextLength = 32;

struct PlatformManifest {
    catalog::SystemName system;
    /// A family of the registry (media_family.hpp), checked where both are read.
    std::string mediaFamily;
    std::string themePack;
    /// The cartridges' shell (and their generated labels' ground), the console's body and its accent.
    std::string cartridgePlastic;
    std::string consolePlastic;
    std::string accentPlastic;
    /// The hero corner's console; none for a room without one (arcade, computer and other systems: the hero corner
    /// stands without its console part).
    std::optional<PrefabRef> console;
    std::string signText;

    bool operator==(const PlatformManifest&) const = default;
};

/// "#RRGGBB" with uppercase hexadecimal digits.
[[nodiscard]] bool isSrgbHexColor(std::string_view text) noexcept;

/// Reads a platform manifest. Errors (context and line): ParseError, ValidationFailed (a key missing, unknown, of
/// another type or form).
[[nodiscard]] Expected<PlatformManifest> parsePlatformManifest(std::string_view text, std::string_view context);
/// Reads <assetRoot>/world/platforms/<system>.json, whose "system" must be `system`. Errors: those of
/// parsePlatformManifest() and of reading the file (NotFound: the system has no manifest of its own).
[[nodiscard]] Expected<PlatformManifest> loadPlatformManifest(const std::filesystem::path& assetRoot,
                                                              const catalog::SystemName& system);

} // namespace rv::world
