#pragma once

// Port IMediaResolver (ADR-0008 §5, specification §4.4): the media files of an entry. Implemented for ES-DE's layout by
// rv_esde (I2 step 8): <media>/<system>/<kind folder>/<mediaStem()>.<ext>, the entry's relative path with the last
// extension of a file's name removed (a folder's name whole), sub-folders mirrored, the file's own name never the
// title. The catalog stores what presence() finds per kind (present, extension) and derives the path again with path()
// when a file is needed.

#include <filesystem>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media.hpp>
#include <rv/core/expected.hpp>

namespace rv::catalog {

/// Read-only towards the media folders. An instance is used from one thread at a time; it may list each system's
/// media folder once and answer from that listing, so a new instance sees files added since. An entry is named by its
/// system, its key (LookupKey::relativePath() as listed) and whether it is a folder (`directory`: FileFacts::directory,
/// true for a Folder entry and for a game that is a folder; false for an entry not seen, a MissingFile), whose media
/// keep its whole name (mediaStem(): "Sports Vol. 2.png", never "Sports Vol.png").
class IMediaResolver {
public:
    virtual ~IMediaResolver() = default;

    /// The media of every kind that the entry has now (also for a MissingFile entry: its media may outlive the ROM).
    /// Errors: Unavailable when nothing can be known: the media root itself is missing or cannot be reached (an
    /// unplugged drive, a share that does not answer), or the system's media folder (or a kind's folder in it) exists
    /// but cannot be listed; the import then keeps the presence it stored. Under a media root that can be reached, a
    /// missing system or kind folder means no media, not an error.
    [[nodiscard]] virtual Expected<EntryMedia> presence(const SystemName& system, const LookupKey& key,
                                                        bool directory) = 0;

    /// The file of `kind` for the entry with the extension `media` holds, derived without looking at the disk.
    /// Errors: NotFound when `media` is not present.
    [[nodiscard]] virtual Expected<std::filesystem::path> path(const SystemName& system, const LookupKey& key,
                                                               bool directory, MediaKind kind,
                                                               const MediaPresence& media) const = 0;
};

} // namespace rv::catalog
