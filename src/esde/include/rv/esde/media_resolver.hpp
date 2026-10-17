#pragma once

// ES-DE's media layout (ADR-0008 §5, specification §4.4): EsdeMediaResolver, catalog::IMediaResolver over ES-DE's media
// folders. ES-DE 3.4.1 finds an entry's media by convention, never through its gamelist (FileData::getMediafilePath,
// getVideoPath, getManualPath): <media root>/<system name>/<kind folder>/<mediaStem() of the entry's relative
// path><extension>, the kind's extensions tried in ES-DE's order (mediaExtensions()), the first file that exists taken.
// That test asks the file system, which compares names as the folder holding them does: case-insensitively with its
// casing table (NTFS, exFAT), unless the folder has case-sensitive names (NTFS's per-folder flag, which WSL sets on the
// folders it makes on Windows drives). So the resolver lists each kind folder of a system once with core::walkTree and
// matches the names with the lookup keys' folding (core::fileSystemUpperCase): the file "covers/collection a/POCKET
// PUZZLE (WORLD).PNG" is the cover of "Collection A/Pocket Puzzle (World).gb", for ES-DE as for Retroverse. Folders
// with case-sensitive names are followed as ES-DE meets them: in a media root or a system folder that has them, only
// the system or kind folder spelled as ES-DE spells it is taken; and a name that matches only through the folding,
// below a folder that has them, is looked up on disk by ES-DE's own spelling (the entry's, with each extension as
// mediaExtensions() spells it). Read-only: folders are listed, no media file is read. The listings stay in memory until
// refresh(), so one import lists each folder once; the catalog stores what presence() finds (present, extension) and
// derives the paths again with path() or mediaRelativePath(), which name a file that is there whatever the folders'
// case rules.
//
// Deliberate differences from ES-DE, both about files ES-DE cannot show either: only files count (ES-DE's
// Utils::FileSystem::exists() is also true for a folder named "Game.png", which it then fails to load), and names are
// matched at any length (ES-DE's plain Win32 paths stop at MAX_PATH unless Windows lets long paths through).

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
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
/// system compares names (".PNG" is ".png", but in a folder with case-sensitive names).
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
    /// The folder's name as the file system spells it ("covers", or "Covers": ES-DE finds either, unless the system's
    /// folder has case-sensitive names); empty when Missing.
    std::string folderName;
    /// Whether the folder has case-sensitive names (NTFS's per-folder flag): ES-DE finds a media file in it only
    /// spelled as the entry's file, with the extension in lower case. Its sub-folders have their own flag, asked when
    /// needed.
    bool caseSensitive = false;
    /// Names (stems) that have a file of the kind, compared folded: one each, whatever the files of the other
    /// extensions beside it.
    std::uint64_t media = 0;
    /// Files beside a file of the same name whose extension ES-DE tries first (the ".jpg" beside a ".png"): never used
    /// (names compared folded; in a folder with case-sensitive names presence() asks the disk).
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
    /// Whether the folder has case-sensitive names: ES-DE then finds only the kind folders spelled as it spells them
    /// ("covers", never "Covers").
    bool caseSensitive = false;
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
/// A system's media folder or a kind folder has case-sensitive names (NTFS's per-folder flag, set by WSL on the folders
/// it makes), or a folder ES-DE would take is spelled otherwise in a folder (the media root included) that has them:
/// ES-DE finds what such a folder holds only spelled exactly as it asks for it. Info.
inline constexpr std::string_view kMediaIssueCaseSensitiveFolder = "media-case-sensitive-folder";

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
    /// A 64-bit FNV-1a hash of that path below the kind folder as the file system spells it (UTF-8, '/' between the
    /// elements): whether an entry spells its media name alike, which no folder's case rule can then refuse.
    std::uint64_t spelling = 0;
};

} // namespace detail

/// What the resolver did so far (since it was made or refreshed).
struct MediaListingStats {
    std::uint64_t systemsListed = 0; ///< system folders listed (Missing and Unavailable ones included)
    std::uint64_t folders = 0;       ///< folders the walks listed below the kind folders (and the kind folders)
    std::uint64_t files = 0;         ///< files the walks listed in the kind folders
    double seconds = 0.0;            ///< wall-clock time spent listing (the media root's check included)
    /// Folders asked whether they have case-sensitive names: the media root, the system and kind folders listed, and
    /// the folders below a kind folder on the way to a media name an entry spells otherwise than the file.
    std::uint64_t caseChecks = 0;
    /// Media files looked up on disk by ES-DE's own spelling: names that matched only through the folding, below a
    /// folder with case-sensitive names.
    std::uint64_t fileChecks = 0;

    bool operator==(const MediaListingStats&) const = default;
};

/// catalog::IMediaResolver over ES-DE's media folders under one media root (resolveMediaRoot(): MediaDirectory, the
/// library's override, or ES-DE's default). Used from one thread at a time. The root is checked on the first question,
/// each system's folder listed on the first question about the system (its kind folders found as ES-DE finds them,
/// each walked once), and the answers come from those listings until refresh(), but for the rare names presence()
/// looks up on disk (below a folder with case-sensitive names, an entry spelled otherwise than its media file).
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
    /// the file system compares names, and its extension as the file spells it. The listing answers, unless the name
    /// matches only through the folding and a folder between the kind folder and the file has case-sensitive names
    /// (or cannot be opened to tell): then each of ES-DE's paths is looked up on disk in ES-DE's order, the extension
    /// as ES-DE spells it. Errors: InvalidArgument for an empty key (an entry outside its system folder has no media),
    /// Unavailable when the root cannot be read (checkRoot()), when the system's folder or one of its kind folders
    /// exists but cannot be listed, when the entry's media would lie below a folder that could not be listed, or when
    /// a lookup on disk fails otherwise than for a missing name.
    [[nodiscard]] Expected<catalog::EntryMedia> presence(const catalog::SystemName& system,
                                                         const catalog::LookupKey& key, bool directory) override;

    /// The file of `kind` (catalog::IMediaResolver::path()): <media root>\mediaRelativePath(), '\' between the
    /// elements, derived without looking at the disk; for what presence() found, a name the file system finds, in
    /// folders with case-sensitive names too. Errors: NotFound when `media` is not present, InvalidArgument for
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
        /// Per kind: the folders below the kind folder asked whether they have case-sensitive names, by folded relative
        /// path (true also for one that could not be opened to tell). Only folders reached through folders without
        /// that flag are asked, so their folded paths name them.
        std::array<std::map<std::string, bool, std::less<>>, catalog::kMediaKindCount> folderCase{};
    };

    [[nodiscard]] Expected<SystemIndex*> index(const catalog::SystemName& system);
    [[nodiscard]] SystemIndex listSystem(const catalog::SystemName& system);
    /// Whether the kind folder or a folder below it on the way to `stem` (a media name as an entry spells it) has
    /// case-sensitive names, or cannot be opened to tell.
    [[nodiscard]] bool caseSensitiveOnTheWay(SystemIndex& index, catalog::MediaKind kind, std::string_view stem);

    std::optional<std::filesystem::path> m_root;
    /// Why the root is not known, for a root resolveMediaRoot() could not resolve.
    std::string m_unresolvedReason;
    /// checkRoot()'s answer once made.
    std::optional<Status> m_rootStatus;
    /// Whether the root has case-sensitive names (checkRoot()): ES-DE then takes only system folders spelled as the
    /// systems.
    bool m_rootCaseSensitive = false;
    /// The root's folders: their folded names to their names as listed (several only in a root with case-sensitive
    /// names, where ES-DE takes only the one spelled like the system).
    std::map<std::string, std::vector<std::string>> m_systemFolders;
    /// The systems listed, by name: map nodes stay put, so listing()'s pointers stay valid.
    std::map<catalog::SystemName, SystemIndex> m_systems;
    MediaListingStats m_stats;
};

} // namespace rv::esde
