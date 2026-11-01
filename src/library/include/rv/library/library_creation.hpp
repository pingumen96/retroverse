#pragma once

// GPU-free (rv_library, ADR-0018 §4, §13): making a library, which retroverse-cli's `library create` and the wizard share.
// draftLibrary() discovers ES-DE and works out what library.json would hold, what the library's folders are and whether
// the user data directory may stand where it does, writing nothing and refusing nothing: what stops the library is a
// DraftProblem, which each caller words for its reader. createLibrary() then writes library.json, the one file written.
// ES-DE's settings are read through esde::readSettings(), whose whitelist never holds the scraper's credentials.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/library.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/system_probe.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/settings.hpp>

namespace rv::core {
class Paths;
} // namespace rv::core

namespace rv::library {

/// The roots a profile gives: its overrides, else ES-DE's settings (nothing when those cannot be read).
struct LibraryRoots {
    std::optional<esde::ResolvedRoot> rom;
    std::optional<esde::ResolvedRoot> media;
    /// Why the settings could not be read, when they could not.
    std::string settingsError;
};

[[nodiscard]] LibraryRoots rootsOf(const catalog::LibraryProfile& profile);

/// The folders of the library a profile describes, which Retroverse only reads (esde::readOnlyFolders()): its ES-DE home
/// and install folder, the roots, the ROM folders of its systems outside them (when the systems can be read).
[[nodiscard]] std::vector<std::filesystem::path> foldersOf(const catalog::LibraryProfile& profile,
                                                           const LibraryRoots& roots);

/// What keeps a draft from becoming a library.
enum class DraftProblem : std::uint8_t {
    None,
    HomeUnknown,         ///< no home folder is known: nothing was found
    HomeMissing,         ///< the ES-DE home found does not exist
    RomRootUnknown,      ///< the ROM root cannot be resolved: an override must give it
    MediaRootUnknown,    ///< the media root likewise
    DataDirectoryInside, ///< the user data directory lies inside the library's folders (ADR-0008 §6, §7)
};
/// "none", "home-unknown", "home-missing", "rom-root-unknown", "media-root-unknown", "data-directory-inside".
[[nodiscard]] std::string_view toString(DraftProblem problem) noexcept;

/// What draftLibrary() is told.
struct LibraryDraftRequest {
    catalog::LibraryId id;
    /// ES-DE's home folder, as ES-DE's own --home takes it: the ES-DE home is its ES-DE folder.
    std::optional<std::filesystem::path> homeFolder;
    /// The ES-DE home itself. Not with homeFolder.
    std::optional<std::filesystem::path> esdeHome;
    std::optional<std::filesystem::path> installFolder;
    /// The overrides of ROMDirectory and MediaDirectory.
    std::optional<std::filesystem::path> romRoot;
    std::optional<std::filesystem::path> mediaRoot;
    /// The user data directory, which the guard keeps out of the library's folders.
    std::filesystem::path userData;
};

struct LibraryDraft {
    esde::EsdeDiscovery discovery;
    /// Nothing without an ES-DE home (HomeUnknown, HomeMissing).
    std::optional<catalog::LibraryProfile> profile;
    LibraryRoots roots;
    /// foldersOf(), with a profile.
    std::vector<std::filesystem::path> folders;
    /// The first thing that keeps it from being a library, in the order above.
    DraftProblem problem = DraftProblem::None;
    /// The data-directory guard's refusal, for DataDirectoryInside.
    std::optional<Error> dataDirectory;
};

/// Discovers ES-DE through `probe` and works out the library. Errors: discovery's (InvalidArgument: a relative explicit
/// path, or both homeFolder and esdeHome). Writes nothing.
[[nodiscard]] Expected<LibraryDraft> draftLibrary(const LibraryDraftRequest& request, const core::ISystemProbe& probe);

/// Writes `draft`'s library.json to `file` (esde::writeLibraryFile()): Conflict when one is there and `existing` keeps
/// it. Errors also: InvalidArgument for a draft with a problem, nothing written.
[[nodiscard]] Status createLibrary(const LibraryDraft& draft, const std::filesystem::path& file,
                                   esde::ExistingFile existing);

/// What a machine answers when it must not be asked (the wizard with --esde-home, ADR-0018 §4): nothing at all, so that
/// discovery stands on the folders given. System32 and Windows are empty paths.
class NoSystemProbe final : public core::ISystemProbe {
public:
    [[nodiscard]] std::optional<std::string> registryString(core::RegistryRoot, std::string_view,
                                                            std::string_view) const override {
        return std::nullopt;
    }
    [[nodiscard]] bool registryKeyExists(core::RegistryRoot, std::string_view) const override { return false; }
    [[nodiscard]] std::vector<std::string> registrySubKeys(core::RegistryRoot, std::string_view) const override {
        return {};
    }
    [[nodiscard]] bool fileExists(const std::filesystem::path&) const override { return false; }
    [[nodiscard]] std::vector<std::filesystem::path> filesMatching(const std::filesystem::path&,
                                                                   std::string_view) const override {
        return {};
    }
    [[nodiscard]] std::optional<std::filesystem::path> homeFolder() const override { return std::nullopt; }
    [[nodiscard]] std::filesystem::path system32Folder() const override { return {}; }
    [[nodiscard]] std::filesystem::path windowsFolder() const override { return {}; }
    [[nodiscard]] std::vector<std::filesystem::path> pathEntries() const override { return {}; }
};

/// How far a data directory has come (ADR-0018 §4: the files say, no record of its own).
enum class LibraryStage : std::uint8_t {
    NoLibrary,  ///< no library.json: the wizard starts on its first page
    NoCatalog,  ///< library.json, but no database, one with no schema yet, or one no import ever finished in: it resumes
                ///< at its preview
    Ready,      ///< a catalog holds the library, or the database cannot be read (the app's notice says so)
};
/// "no-library", "no-catalog", "ready".
[[nodiscard]] std::string_view toString(LibraryStage stage) noexcept;

/// Where `paths`' data directory stands. Reads library.json's presence and, read-only, whether the database holds a
/// system; never makes or migrates the database.
[[nodiscard]] LibraryStage libraryStage(const core::Paths& paths);

} // namespace rv::library
