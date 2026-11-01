#include <rv/library/library_creation.hpp>

#include <algorithm>
#include <system_error>
#include <utility>

#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/core/paths.hpp>
#include <rv/esde/systems.hpp>

namespace rv::library {

std::string_view toString(DraftProblem problem) noexcept {
    switch (problem) {
    case DraftProblem::None:
        return "none";
    case DraftProblem::HomeUnknown:
        return "home-unknown";
    case DraftProblem::HomeMissing:
        return "home-missing";
    case DraftProblem::RomRootUnknown:
        return "rom-root-unknown";
    case DraftProblem::MediaRootUnknown:
        return "media-root-unknown";
    case DraftProblem::DataDirectoryInside:
        return "data-directory-inside";
    }
    return "unknown";
}

LibraryRoots rootsOf(const catalog::LibraryProfile& profile) {
    LibraryRoots roots;
    const auto settings = esde::readSettings(profile.esdeHome);
    if (settings) {
        roots.rom = esde::resolveRomRoot(*settings, profile);
        roots.media = esde::resolveMediaRoot(*settings, profile);
        return roots;
    }
    // Overrides need no settings.
    roots.settingsError = settings.error().toString();
    const esde::EsdeSettings none;
    if (profile.romRoot) {
        roots.rom = esde::resolveRomRoot(none, profile);
    }
    if (profile.mediaRoot) {
        roots.media = esde::resolveMediaRoot(none, profile);
    }
    return roots;
}

std::vector<std::filesystem::path> foldersOf(const catalog::LibraryProfile& profile, const LibraryRoots& roots) {
    const auto systems = esde::loadSystems(profile);
    return esde::readOnlyFolders(profile, roots.rom, roots.media, systems ? &*systems : nullptr);
}

Expected<LibraryDraft> draftLibrary(const LibraryDraftRequest& request, const core::ISystemProbe& probe) {
    esde::DiscoveryOptions options;
    options.homeFolder = request.homeFolder;
    options.esdeHome = request.esdeHome;
    options.installFolder = request.installFolder;
    auto discovery = esde::discoverEsde(options, probe);
    if (!discovery) {
        return propagate(discovery.error());
    }
    LibraryDraft draft;
    draft.discovery = std::move(*discovery);
    if (!draft.discovery.esdeHome) {
        draft.problem = DraftProblem::HomeUnknown;
        return draft;
    }
    if (!draft.discovery.esdeHomeExists) {
        draft.problem = DraftProblem::HomeMissing;
        return draft;
    }
    auto profile = esde::profileOf(draft.discovery, request.id);
    if (!profile) {
        return propagate(profile.error());
    }
    profile->romRoot = request.romRoot;
    profile->mediaRoot = request.mediaRoot;
    draft.roots = rootsOf(*profile);
    draft.folders = foldersOf(*profile, draft.roots);
    draft.profile = std::move(*profile);
    // Retroverse writes nowhere in what it reads (ADR-0008 §6, §7): the roots must be known to be kept out of.
    if (!draft.roots.rom || !draft.roots.rom->path) {
        draft.problem = DraftProblem::RomRootUnknown;
    } else if (!draft.roots.media || !draft.roots.media->path) {
        draft.problem = DraftProblem::MediaRootUnknown;
    } else if (const Status allowed = core::checkUserDataDirectory(request.userData, draft.folders); !allowed) {
        draft.problem = DraftProblem::DataDirectoryInside;
        draft.dataDirectory = allowed.error();
    }
    return draft;
}

Status createLibrary(const LibraryDraft& draft, const std::filesystem::path& file, esde::ExistingFile existing) {
    if (draft.problem != DraftProblem::None || !draft.profile) {
        return makeError(ErrorCode::InvalidArgument, "the library cannot be made", std::string(toString(draft.problem)));
    }
    return esde::writeLibraryFile(file, *draft.profile, existing);
}

std::string_view toString(LibraryStage stage) noexcept {
    switch (stage) {
    case LibraryStage::NoLibrary:
        return "no-library";
    case LibraryStage::NoCatalog:
        return "no-catalog";
    case LibraryStage::Ready:
        return "ready";
    }
    return "unknown";
}

LibraryStage libraryStage(const core::Paths& paths) {
    std::error_code ignored;
    if (!std::filesystem::exists(paths.configDir() / esde::kLibraryFileName, ignored)) {
        return LibraryStage::NoLibrary;
    }
    const std::filesystem::path file = catalog_db::databaseFile(paths);
    if (!std::filesystem::exists(file, ignored)) {
        return LibraryStage::NoCatalog;
    }
    catalog_db::DatabaseOptions options;
    options.create = false;
    options.switchToWal = false; // reading writes nothing into the file
    auto database = catalog_db::Database::open(file, options);
    if (!database) {
        return LibraryStage::Ready;
    }
    auto reader = database->openReader();
    if (!reader) {
        return LibraryStage::Ready;
    }
    auto query = catalog_db::SqliteCatalogQuery::open(std::move(*reader));
    if (!query) {
        return LibraryStage::Ready; // an older schema, another version: the catalog's own opening says what to do
    }
    auto systems = (*query)->systems();
    return systems && systems->empty() ? LibraryStage::NoCatalog : LibraryStage::Ready;
}

} // namespace rv::library
