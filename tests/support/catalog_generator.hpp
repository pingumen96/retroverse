#pragma once

// A generated catalog for the query and search tests and benchmarks (I2 step 9): invented titles in several languages,
// with diacritics (precomposed, and decomposed for some entries), of one to five words, with sort titles; namesakes
// across systems and within one (ties of the sort key); every indexed entry state (Game, MissingFile, NotAGame with
// each reason, DiscMember, Folder) and availability (a system whose folder is gone holds SourceUnavailable games);
// unscraped files titled from their names; developers, publishers, genres, release dates (some undated), players,
// ratings, favorites, media; descriptions holding a word nothing else does (kDescriptionOnlyWord: descriptions are not
// searched). Deterministic: the same options give the same catalog, GameIds included (stableGameId() of each entry's
// key, as a first import assigns them). ES-DE's own system names; titles, companies and the extra systems invented.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/catalog_store.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/system.hpp>
#include <rv/core/expected.hpp>

namespace rv::test {

/// A word only the generated descriptions hold.
inline constexpr std::string_view kDescriptionOnlyWord = "quixotry";

/// What a generated catalog holds.
struct CatalogGeneratorOptions {
    /// Indexed entries in all, of every state (about 84% of them Available games).
    std::size_t entries = 10'000;
    /// The seed of every choice.
    std::uint64_t seed = 9;
    /// The library's id.
    std::string library = "esde";
};

/// A generated catalog: its library, its systems and its entries.
struct GeneratedCatalog {
    catalog::LibraryProfile library;
    /// generatedSystems(), in that order.
    std::vector<catalog::SystemInfo> systems;
    /// Every entry, in ascending GameId order (the order they are written in); empty when not kept
    /// (generateCatalogDatabase() with keepEntries off).
    std::vector<catalog::IndexedEntry> entries;
    /// The number of entries written.
    std::size_t entryCount = 0;
};

/// The library of a generated catalog.
[[nodiscard]] catalog::LibraryProfile generatedLibrary(const CatalogGeneratorOptions& options);

/// The systems of a generated catalog, the same whatever its size: ES-DE systems, an invented one whose full name has
/// diacritics ("fictif", "Système Fictif (Éditions Pixel)"), and "archive", whose folder is missing (SourceUnavailable,
/// FolderMissing), last.
[[nodiscard]] std::vector<catalog::SystemInfo> generatedSystems();

/// Entry `index` of a generated catalog (0 <= index < options.entries), with its GameId (counter 0), media and sort key.
[[nodiscard]] catalog::IndexedEntry generatedEntry(const CatalogGeneratorOptions& options, std::size_t index);

/// Every entry of the catalog, in ascending GameId order. Errors: InvalidArgument when two entries' GameIds collide
/// (which a different seed avoids).
[[nodiscard]] Expected<GeneratedCatalog> generateCatalog(const CatalogGeneratorOptions& options);

/// Writes the catalog through `store` in one import transaction: the library, the systems, then each entry's identity
/// and index rows, in the order of `catalog.entries`.
[[nodiscard]] Status writeCatalog(catalog::ICatalogStore& store, const GeneratedCatalog& catalog);

/// Creates the catalog database at `file` (openCatalogDatabase(): made and migrated) holding the generated catalog, in
/// one import transaction, entries written in ascending GameId order; the database is closed when it returns. With
/// `keepEntries` off, the entries are generated again as they are written instead of being kept (100,000 entries hold
/// about 200 MB): the result's entries are then empty. Errors: those of generateCatalog() and of the store.
[[nodiscard]] Expected<GeneratedCatalog> generateCatalogDatabase(const std::filesystem::path& file,
                                                                 const CatalogGeneratorOptions& options,
                                                                 bool keepEntries = true);

/// The search folding of the text the generator writes, for test oracles: ASCII lower case, the generator's accented
/// letters without their diacritics (as FTS5's unicode61 with remove_diacritics 2 folds them), decomposed accents
/// dropped; then the words, runs of ASCII letters and digits. Only meant for generated text.
[[nodiscard]] std::vector<std::string> generatedWords(std::string_view text);

} // namespace rv::test
