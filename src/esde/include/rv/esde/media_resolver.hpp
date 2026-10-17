#pragma once

// ES-DE's media layout (ADR-0008 §5, specification §4.4): EsdeMediaResolver, catalog::IMediaResolver over ES-DE's media
// folders. ES-DE 3.4.1 finds an entry's media by convention, never through its gamelist (FileData::getMediafilePath,
// getVideoPath, getManualPath): <media root>/<system name>/<kind folder>/<mediaStem() of the entry's relative
// path><extension>, the kind's extensions tried in ES-DE's order (mediaExtensions()), the first file that exists taken.
// On Windows that test compares names as the file system does, so the resolver lists each kind folder of a system once
// with core::walkTree and matches the names with the lookup keys' folding (core::fileSystemUpperCase): the file
// "covers/collection a/POCKET PUZZLE (WORLD).PNG" is the cover of "Collection A/Pocket Puzzle (World).gb", for ES-DE
// as for Retroverse. Read-only: folders are listed, no media file is opened. The listings stay in memory until
// refresh(), so one import lists each folder once; the catalog stores what presence() finds (present, extension) and
// derives the paths again with path() or mediaRelativePath().
//
// Deliberate differences from ES-DE, all about files ES-DE cannot show either: only files count (ES-DE's
// Utils::FileSystem::exists() is also true for a folder named "Game.png", which it then fails to load), names are
// matched at any length (ES-DE's plain Win32 paths stop at MAX_PATH unless Windows lets long paths through), and of two
// files whose names differ in case only (a case-sensitive folder) the one spelled with ES-DE's extension is taken, then
// the first in ordinal order, where ES-DE would ask for its own spelling of the stem too.

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/media_resolver.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/core/expected.hpp>
#include <rv/esde/settings.hpp>

namespace rv::esde {

/// The extensions ES-DE 3.4.1 reads for the image kinds, in the order it tries them (FileData::sImageExtensions): every
/// kind but videos and manuals, custom included.
inline constexpr std::array<std::string_view, 3> kImageExtensions{".png", ".jpg", ".webp"};
/// The video extensions, in ES-DE's order (FileData::sVideoExtensions). Nothing else is a video for ES-DE: an ".flv"
/// in a videos folder is never shown.
inline constexpr std::array<std::string_view, 6> kVideoExtensions{".mp4", ".mkv", ".avi", ".wmv", ".mov", ".webm"};
/// The manual extension (FileData::getManualPath).
inline constexpr std::array<std::string_view, 1> kManualExtensions{".pdf"};

/// The extensions ES-DE reads for `kind`, in the order it tries them: the first that exists wins. Compared as the file
/// system compares names (".PNG" is ".png").
[[nodiscard]] std::span<const std::string_view> mediaExtensions(catalog::MediaKind kind) noexcept;

/// The path below the media root of an entry's file of `kind` with `extension` (".png"): "<system>/<kind
/// folder>/<mediaStem(key.relativePath(), directory)><extension>", '/' between the elements, the names as ES-DE builds
/// them (the system's name, the kind's folder, the entry's relative path as listed on disk). Reopen it through
/// core::extendedLengthPathBelow(media root, it), which reaches names that end with a dot or a space.
[[nodiscard]] std::string mediaRelativePath(const catalog::SystemName& system, const catalog::LookupKey& key,
                                            bool directory, catalog::MediaKind kind, std::string_view extension);

/// Whether a media folder could be listed.
enum class MediaFolderState : std::uint8_t {
    Listed,      ///< listed: what it holds is known, but below the folders MediaKindListing::unlistedFolders names
    Missing,     ///< not there (or a file of that name): no media, which is no error
    Unavailable, ///< there, but it cannot be listed (access denied, an I/O error, a link that loops) or its drive cannot
                 ///< be reached: nothing is known about its media
};

/// "listed", "missing", "unavailable".
[[nodiscard]] std::string_view toString(MediaFolderState state) noexcept;

/// The files of one extension that ES-DE never uses in a kind's folder.
struct IgnoredExtension {
    /// The extension as the files spell it (".flv"); empty for names without one.
    std::string extension;
    std::uint64_t files = 0;

    bool operator==(const IgnoredExtension&) const = default;
};

/// One kind folder of a system, as listed.
struct MediaKindListing {
    MediaFolderState state = MediaFolderState::Missing;
    /// The folder's name as the file system spells it ("covers", or "Covers": ES-DE finds either); empty when Missing.
    std::string folderName;
    /// Names (stems) that have a file of the kind: one each, whatever the files of the other extensions beside it.
    std::uint64_t media = 0;
    /// Files beside a file of the same name whose extension ES-DE tries first (the ".jpg" beside a ".png"): never used.
    std::uint64_t shadowedFiles = 0;
    /// Files of extensions ES-DE does not read for the kind, by extension as spelled (ordinal); a name without an
    /// extension under "". Never used.
    std::vector<IgnoredExtension> ignored;
    /// Entries the walk reported but could not read (a link whose target cannot be opened, a name without a UTF-8
    /// form): files ES-DE cannot open either, counted as no media.
    std::uint64_t unreadableEntries = 0;
    /// The folders below it that could not be listed, relative to it ('/' between the elements, as listed): whether an
    /// entry whose media would lie below one of them has media is not known (presence() is Unavailable for it).
    std::vector<std::string> unlistedFolders;
    /// What the file system said, for Unavailable.
    std::string detail;

    /// The files of ignored extensions.
    [[nodiscard]] std::uint64_t ignoredFiles() const noexcept;

    bool operator==(const MediaKindListing&) const = default;
};

/// A system's media folder, as listed: <media root>/<system name>, with its kind folders.
struct SystemMediaListing {
    catalog::SystemName system;
    MediaFolderState state = MediaFolderState::Missing;
    /// The folder's name as the file system spells it; empty when Missing.
    std::string folderName;
    /// One per kind, in the order of catalog::kMediaKinds (Missing ones too).
    std::array<MediaKindListing, catalog::kMediaKindCount> kinds{};
    /// What the file system said, for Unavailable.
    std::string detail;
    /// What the listing found that ES-DE never shows or that could not be read (kMediaIssue... codes), in the order of
    /// the kinds.
    std::vector<catalog::Issue> issues;

    [[nodiscard]] const MediaKindListing& operator[](catalog::MediaKind kind) const noexcept {
        return kinds[static_cast<std::size_t>(kind)];
    }
    /// The media of every kind (MediaKindListing::media summed).
    [[nodiscard]] std::uint64_t media() const noexcept;

    bool operator==(const SystemMediaListing&) const = default;
};

/// The codes of the issues of the media listings (catalog::Issue::code), each naming the system and the folder.
/// The media root is not known (unresolved), missing or cannot be reached: nothing can be known about any media. Error.
inline constexpr std::string_view kMediaIssueRootUnavailable = "media-root-unavailable";
/// A system's media folder, a kind folder or a folder below one exists but cannot be listed: the presence of the media
/// it would hold is not known, and the import keeps what it stored. Error.
inline constexpr std::string_view kMediaIssueFolderUnlisted = "media-folder-unlisted";
/// Files ES-DE never uses in a kind's folder: an extension it does not read for the kind (".flv" videos), one per
/// extension with the count. Info.
inline constexpr std::string_view kMediaIssueIgnoredExtension = "media-ignored-extension";
/// Files beside a file of the same name whose extension ES-DE tries first (a ".jpg" beside a ".png"), per kind with the
/// count. Info.
inline constexpr std::string_view kMediaIssueShadowedFile = "media-shadowed-file";
/// Entries of a kind's folder the walk could not read (a link whose target cannot be opened, a name without a UTF-8
/// form), per kind with the count: no media, as ES-DE cannot open them either. Warning.
inline constexpr std::string_view kMediaIssueUnreadableEntry = "media-unreadable-entry";

namespace detail {

/// A file of a kind folder that is media of the kind, by the media name it serves (EsdeMediaResolver's index).
struct MediaFile {
    /// mediaStem() of its path below the kind folder, folded as lookup keys are ("COLLECTION A/POCKET PUZZLE (WORLD)").
    std::string name;
    /// The extension as the file spells it (".png").
    std::string extension;
    /// Its place in mediaExtensions(): the lowest wins.
    std::uint8_t rank = 0;
    /// Spelled exactly as ES-DE spells the extension: the tie-break between names that differ in case only.
    bool exactExtension = true;
};

} // namespace detail

/// What the resolver did so far (since it was made or refreshed).
struct MediaListingStats {
    std::uint64_t systemsListed = 0; ///< system folders listed (Missing and Unavailable ones included)
    std::uint64_t folders = 0;       ///< folders the walks listed below the kind folders (and the kind folders)
    std::uint64_t files = 0;         ///< files the walks listed in the kind folders
    double seconds = 0.0;            ///< wall-clock time spent listing (the media root's check included)

    bool operator==(const MediaListingStats&) const = default;
};

/// catalog::IMediaResolver over ES-DE's media folders under one media root (resolveMediaRoot(): MediaDirectory, the
/// library's override, or ES-DE's default). Used from one thread at a time. The root is checked on the first question,
/// each system's folder listed on the first question about the system (its kind folders found case-insensitively, as
/// ES-DE finds them, each walked once), and the answers come from those listings until refresh().
class EsdeMediaResolver final : public catalog::IMediaResolver {
public:
    /// The media under `mediaRoot`, an absolute folder (ResolvedRoot::path). Nothing is read before the first question.
    explicit EsdeMediaResolver(std::filesystem::path mediaRoot);
    /// The media under the root resolveMediaRoot() gives; a root it could not resolve makes every question Unavailable
    /// with the root's issue as the reason.
    explicit EsdeMediaResolver(const ResolvedRoot& mediaRoot);

    /// The media root; nothing when it was not resolved.
    [[nodiscard]] const std::optional<std::filesystem::path>& mediaRoot() const noexcept { return m_root; }

    /// Whether the media root can be read: checked once, until refresh(). Errors: Unavailable (the root is not known,
    /// missing, a file, its drive or share cannot be reached, or it cannot be listed), the reason naming the root.
    [[nodiscard]] Status checkRoot();

    /// The listing of the media folder of `system`, made on the first call for it (and kept until refresh()); the
    /// pointer stays valid until refresh() or the resolver goes. A system without a media folder is Missing, one whose
    /// folder cannot be listed Unavailable: neither is an error. Errors: those of checkRoot().
    [[nodiscard]] Expected<const SystemMediaListing*> listing(const catalog::SystemName& system);

    /// The media of every kind the entry has (catalog::IMediaResolver::presence()): for each kind, the first of
    /// mediaExtensions() for which a file of the entry's media name (mediaStem()) is in the kind's folder, compared as
    /// the file system compares names, and its extension as the file spells it. Errors: InvalidArgument for an empty
    /// key (an entry outside its system folder has no media), Unavailable when the root cannot be read (checkRoot()),
    /// when the system's folder or one of its kind folders exists but cannot be listed, or when the entry's media would
    /// lie below a folder that could not be listed.
    [[nodiscard]] Expected<catalog::EntryMedia> presence(const catalog::SystemName& system,
                                                         const catalog::LookupKey& key, bool directory) override;

    /// The file of `kind` (catalog::IMediaResolver::path()): <media root>\mediaRelativePath(), '\' between the
    /// elements, derived without looking at the disk. Errors: NotFound when `media` is not present, InvalidArgument for
    /// an empty key or an extension that is no extension (empty, without its leading dot, or holding a separator, ':'
    /// or a character below U+0020), Unavailable when the media root is not known.
    [[nodiscard]] Expected<std::filesystem::path> path(const catalog::SystemName& system,
                                                       const catalog::LookupKey& key, bool directory,
                                                       catalog::MediaKind kind,
                                                       const catalog::MediaPresence& media) const override;

    /// Forgets the root's check and every listing: the next question reads the folders again and sees the files added
    /// or removed meanwhile. The statistics start again too.
    void refresh() noexcept;

    /// What was listed since the resolver was made or refreshed.
    [[nodiscard]] const MediaListingStats& stats() const noexcept { return m_stats; }

private:
    struct SystemIndex {
        SystemMediaListing listing;
        /// Per kind: the files that are media, by name (ordinal, one per name), and the folded relative paths of the
        /// folders that could not be listed.
        std::array<std::vector<detail::MediaFile>, catalog::kMediaKindCount> files{};
        std::array<std::vector<std::string>, catalog::kMediaKindCount> unlistedFolded{};
    };

    [[nodiscard]] Expected<const SystemIndex*> index(const catalog::SystemName& system);
    [[nodiscard]] SystemIndex listSystem(const catalog::SystemName& system);

    std::optional<std::filesystem::path> m_root;
    /// Why the root is not known, for a root resolveMediaRoot() could not resolve.
    std::string m_unresolvedReason;
    /// checkRoot()'s answer once made.
    std::optional<Status> m_rootStatus;
    /// The root's folders: their folded names to their names as listed (of names that differ in case only, the one
    /// spelled like a system name wins when that one is asked for).
    std::map<std::string, std::vector<std::string>> m_systemFolders;
    /// The systems listed, by name: map nodes stay put, so listing()'s pointers stay valid.
    std::map<catalog::SystemName, SystemIndex> m_systems;
    MediaListingStats m_stats;
};

} // namespace rv::esde
