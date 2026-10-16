#pragma once

// Media kinds and the media an entry has (specification §4.4, ADR-0008 §5). ES-DE finds media by convention, never
// through the gamelist: <media>/<system>/<kind folder>/<mediaStem() of the entry's relative path>.<ext>. The catalog
// keeps per entry and kind only whether a file is there and its extension; the path is derived again when needed
// (IMediaResolver).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <rv/catalog/ids.hpp>

namespace rv::catalog {

/// The media kinds ES-DE recognizes (specification §4.4). The values are stored (as numbers and as bits of a presence
/// mask): never reorder them, add new ones at the end.
enum class MediaKind : std::uint8_t {
    Cover,         ///< covers
    BackCover,     ///< backcovers
    Box3d,         ///< 3dboxes
    PhysicalMedia, ///< physicalmedia
    Screenshot,    ///< screenshots
    TitleScreen,   ///< titlescreens
    Marquee,       ///< marquees
    MixImage,      ///< miximages
    FanArt,        ///< fanart
    Manual,        ///< manuals
    Video,         ///< videos
    Custom,        ///< custom
};

inline constexpr std::size_t kMediaKindCount = 12;

/// Every kind, in the order of their values.
inline constexpr std::array<MediaKind, kMediaKindCount> kMediaKinds{
    MediaKind::Cover,      MediaKind::BackCover,   MediaKind::Box3d,   MediaKind::PhysicalMedia,
    MediaKind::Screenshot, MediaKind::TitleScreen, MediaKind::Marquee, MediaKind::MixImage,
    MediaKind::FanArt,     MediaKind::Manual,      MediaKind::Video,   MediaKind::Custom,
};

/// ES-DE's folder of the kind under <media>/<system>: "covers", "backcovers", "3dboxes", "physicalmedia",
/// "screenshots", "titlescreens", "marquees", "miximages", "fanart", "manuals", "videos", "custom". Also the kind's
/// name in text (the CLI, the store).
[[nodiscard]] constexpr std::string_view toString(MediaKind kind) noexcept {
    switch (kind) {
    case MediaKind::Cover:
        return "covers";
    case MediaKind::BackCover:
        return "backcovers";
    case MediaKind::Box3d:
        return "3dboxes";
    case MediaKind::PhysicalMedia:
        return "physicalmedia";
    case MediaKind::Screenshot:
        return "screenshots";
    case MediaKind::TitleScreen:
        return "titlescreens";
    case MediaKind::Marquee:
        return "marquees";
    case MediaKind::MixImage:
        return "miximages";
    case MediaKind::FanArt:
        return "fanart";
    case MediaKind::Manual:
        return "manuals";
    case MediaKind::Video:
        return "videos";
    case MediaKind::Custom:
        return "custom";
    }
    return "unknown";
}

/// The kind whose ES-DE folder is `folder` (exactly, as ES-DE names it).
[[nodiscard]] constexpr std::optional<MediaKind> parseMediaKind(std::string_view folder) noexcept {
    return detail::enumFromString(folder, MediaKind::Custom);
}

/// What an entry's media files are named, below <media>/<system>/<kind folder>: its relative path (a
/// LookupKey::relativePath(), '/' between the elements) with the last extension of a file's name removed, from the last
/// dot of the last element ("Collection A/Pocket Puzzle (World).gb" gives "Collection A/Pocket Puzzle (World)",
/// "Game (Europe).dec.iso" gives "Game (Europe).dec", a name without a dot stays), and whole for a folder, a Folder
/// entry or a game that is a folder (`directory`: "Sports Vol. 2" stays "Sports Vol. 2"): ES-DE names media after
/// FileData::getDisplayName(), whose stem keeps a directory's name whole. Never trimmed: "Galactic Disk .atr" gives
/// "Galactic Disk ", and a file named ".gb" an empty name ("Sub/.gb" gives "Sub/"), as ES-DE builds them.
[[nodiscard]] inline std::string mediaStem(std::string_view relativePath, bool directory) {
    std::string stem(relativePath);
    if (!directory) {
        const std::size_t slash = stem.rfind('/');
        const std::size_t nameStart = slash == std::string::npos ? 0 : slash + 1;
        if (const std::size_t dot = stem.rfind('.'); dot != std::string::npos && dot >= nameStart) {
            stem.erase(dot);
        }
    }
    return stem;
}

/// Whether an entry has a file of one kind.
struct MediaPresence {
    bool present = false;
    /// The file's extension with its dot (".png"), as found; empty when absent.
    std::string extension;

    bool operator==(const MediaPresence&) const = default;
};

/// The media of an entry: one MediaPresence per kind.
struct EntryMedia {
    std::array<MediaPresence, kMediaKindCount> kinds{};

    [[nodiscard]] const MediaPresence& operator[](MediaKind kind) const noexcept {
        return kinds[static_cast<std::size_t>(kind)];
    }
    [[nodiscard]] MediaPresence& operator[](MediaKind kind) noexcept { return kinds[static_cast<std::size_t>(kind)]; }

    /// Bit n set when the kind of value n is present (a compact form for storage and filters).
    [[nodiscard]] std::uint32_t presenceMask() const noexcept {
        std::uint32_t mask = 0;
        for (std::size_t i = 0; i < kMediaKindCount; ++i) {
            if (kinds[i].present) {
                mask |= 1u << i;
            }
        }
        return mask;
    }
    [[nodiscard]] bool any() const noexcept { return presenceMask() != 0; }

    bool operator==(const EntryMedia&) const = default;
};

} // namespace rv::catalog
