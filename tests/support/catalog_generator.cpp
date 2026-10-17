#include "support/catalog_generator.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>

#include <fmt/format.h>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/identity.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/catalog_db/search_index.hpp>
#include <rv/core/text.hpp>

namespace rv::test {
namespace {

using catalog::Availability;
using catalog::EntryState;

/// SplitMix64: a small, fast and well-mixed generator, seeded per entry so that any entry can be made alone.
class Random {
public:
    explicit Random(std::uint64_t seed) noexcept : m_state(seed) {}

    std::uint64_t next() noexcept {
        std::uint64_t z = (m_state += 0x9E37'79B9'7F4A'7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBull;
        return z ^ (z >> 31);
    }
    /// 0 to bound - 1.
    std::size_t below(std::size_t bound) noexcept { return static_cast<std::size_t>(next() % bound); }
    /// True `percent` times in a hundred.
    bool chance(unsigned percent) noexcept { return below(100) < percent; }

    template <class Array>
    auto pick(const Array& values) noexcept {
        return values[below(values.size())];
    }

private:
    std::uint64_t m_state;
};

/// The generator of one entry's (stream 1) or one work's (stream 2) choices.
Random randomFor(const CatalogGeneratorOptions& options, std::uint64_t stream, std::uint64_t index) noexcept {
    Random mixer(options.seed ^ (stream << 56) ^ index);
    return Random(mixer.next());
}

constexpr std::array<std::string_view, 28> kAdjectives{
    "Super", "Mega",   "Hyper",   "Little",  "Crimson", "Silent", "Golden", "Électrique", "Fantôme", "Brûlant",
    "Glacé", "Über",   "Naïve",   "Phantom", "Neon",    "Lunar",  "Solar",  "Grande",     "Tiny",    "Wild",
    "Royal", "Mystic", "Dernier", "Pequeño", "Rápido",  "Noir",   "Blitz",  "Señorial",
};

constexpr std::array<std::string_view, 50> kNouns{
    "Pixel",  "Quest",     "Racer",  "Harbor",  "Castle", "Dragon", "Éclair",    "Château", "Façade",   "Garçon",
    "Ninja",  "Rocket",    "Puzzle", "Island",  "Comet",  "Knight", "Tower",     "Jungle",  "Piñata",   "Señor",
    "Café",   "Ångström",  "Kōbō",   "Crème",   "Forêt",  "Rêve",   "Mañana",    "Zürich",  "Göteborg", "Fjäll",
    "Tennis", "Golf",      "Soccer", "Hockey",  "Galaxy", "Robot",  "Wizard",    "Samurai", "Pirate",   "Circus",
    "Garden", "Labyrinth", "Voyage", "Odyssey", "Saga",   "Legend", "Chronicle", "Blaster", "Striker",  "Runner",
};

constexpr std::array<std::string_view, 7> kNumbers{"2", "3", "II", "III", "64", "2000", "Zero"};

constexpr std::array<std::string_view, 12> kCompanies{
    "Mirage Soft", "Éditions Pixel", "Kumo Works",    "Studio Brûlé",  "Nordlys Interaktiv", "Señal Games",
    "Bitwerk",     "Ostrava Labs",   "Harbor Lights", "Café Logiciel", "Tanuki Arts",        "Zürcher Spielwerk",
};

constexpr std::array<std::string_view, 9> kGenres{
    "Action", "Platform",    "Puzzle",
    "Racing", "Shoot'em up", "Role Playing Game",
    "Sport",  "Stratégie",   "Corsa, Guida / Moto",
};

constexpr std::array<std::string_view, 6> kRegions{"Europe", "USA", "Japan", "World", "France", "España"};

/// A system of the generated catalog, with its share of the entries (in percent) and its file extension.
struct GeneratedSystem {
    std::string_view name;
    std::string_view fullName;
    std::string_view extension;
    unsigned share;
};

constexpr std::array<GeneratedSystem, 11> kSystems{{
    {"gb", "Nintendo Game Boy", ".gb", 12},
    {"snes", "Nintendo SNES (Super Nintendo)", ".sfc", 14},
    {"psx", "Sony PlayStation", ".chd", 16},
    {"megadrive", "Sega Mega Drive", ".md", 12},
    {"n64", "Nintendo 64", ".z64", 6},
    {"arcade", "Arcade", ".zip", 10},
    {"pcengine", "NEC PC Engine", ".pce", 6},
    {"msx", "MSX", ".rom", 4},
    {"amiga", "Commodore Amiga", ".adf", 6},
    {"fictif", "Système Fictif (Éditions Pixel)", ".fic", 8},
    {"archive", "Archivio Remoto", ".zip", 6},
}};

constexpr std::size_t kArchiveSystem = kSystems.size() - 1;
constexpr std::size_t kDiscSystem = 2; // psx

/// The system an entry belongs to, by the systems' shares.
std::size_t pickSystem(Random& random) noexcept {
    std::size_t roll = random.below(100);
    for (std::size_t i = 0; i < kSystems.size(); ++i) {
        if (roll < kSystems[i].share) {
            return i;
        }
        roll -= kSystems[i].share;
    }
    return 0;
}

/// A work: a title and the sort title it may carry. Entries sharing a work are namesakes (in one system or several).
struct Work {
    std::string title;
    std::string sortName;
};

Work makeWork(const CatalogGeneratorOptions& options, std::size_t work) {
    Random random = randomFor(options, 2, work);
    Work result;
    switch (random.below(10)) {
    case 0:
    case 1:
    case 2:
    case 3:
        result.title = fmt::format("{} {}", random.pick(kAdjectives), random.pick(kNouns));
        break;
    case 4:
        result.title = std::string(random.pick(kNouns));
        break;
    case 5:
        result.title = fmt::format("{} {}", random.pick(kNouns), random.pick(kNouns));
        break;
    case 6: {
        const std::string_view first = random.pick(kNouns);
        const std::string_view second = random.pick(kNouns);
        result.title = fmt::format("The {} of {}", first, second);
        result.sortName = fmt::format("{} of {}, The", first, second);
        break;
    }
    case 7:
        result.title = fmt::format("{} {} {}", random.pick(kAdjectives), random.pick(kNouns), random.pick(kNumbers));
        break;
    case 8:
        result.title = fmt::format("{}: {} {}", random.pick(kNouns), random.pick(kAdjectives), random.pick(kNouns));
        break;
    default:
        result.title = fmt::format("{} {} and the {} {}", random.pick(kAdjectives), random.pick(kNouns),
                                   random.pick(kAdjectives), random.pick(kNouns));
        break;
    }
    return result;
}

/// The precomposed accented letters that decomposed() decomposes, each with its decomposition (a base letter and
/// U+0301, U+0300, U+0302, U+0308, U+0303, U+0327, U+030A, U+0304), as text written in NFD holds them.
constexpr std::array<std::pair<std::string_view, std::string_view>, 30> kDecompositions{{
    {"é", "e\xCC\x81"}, {"É", "E\xCC\x81"}, {"è", "e\xCC\x80"}, {"ê", "e\xCC\x82"}, {"ë", "e\xCC\x88"},
    {"à", "a\xCC\x80"}, {"â", "a\xCC\x82"}, {"ä", "a\xCC\x88"}, {"á", "a\xCC\x81"}, {"å", "a\xCC\x8A"},
    {"Å", "A\xCC\x8A"}, {"ç", "c\xCC\xA7"}, {"ñ", "n\xCC\x83"}, {"ü", "u\xCC\x88"}, {"Ü", "U\xCC\x88"},
    {"ö", "o\xCC\x88"}, {"ô", "o\xCC\x82"}, {"ō", "o\xCC\x84"}, {"û", "u\xCC\x82"}, {"î", "i\xCC\x82"},
    {"ï", "i\xCC\x88"}, {"í", "i\xCC\x81"}, {"ó", "o\xCC\x81"}, {"ú", "u\xCC\x81"}, {"ù", "u\xCC\x80"},
    {"ã", "a\xCC\x83"}, {"õ", "o\xCC\x83"}, {"ÿ", "y\xCC\x88"}, {"ò", "o\xCC\x80"}, {"ì", "i\xCC\x80"},
}};

/// `text` with every letter of kDecompositions replaced by its decomposition (`decompose`), or every decomposition by
/// its letter.
std::string replacedLetters(std::string_view text, bool decompose) {
    std::string result;
    std::size_t at = 0;
    while (at < text.size()) {
        bool replaced = false;
        for (const auto& [letter, parts] : kDecompositions) {
            const std::string_view from = decompose ? letter : parts;
            if (text.substr(at).starts_with(from)) {
                result += decompose ? parts : letter;
                at += from.size();
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            result += text[at++];
        }
    }
    return result;
}

/// `text` with its precomposed accented letters decomposed, as text written in NFD holds them.
std::string decomposed(std::string_view text) {
    return replacedLetters(text, true);
}

/// A file name stem from a title: what a file system takes (no ':').
std::string stemOf(std::string_view title) {
    std::string stem;
    for (const char c : title) {
        if (c == ':') {
            stem += " -";
        } else {
            stem += c;
        }
    }
    return stem;
}

catalog::GameId gameIdOf(const catalog::LibraryId& library, const catalog::CatalogEntry& entry) {
    for (std::uint32_t counter = 0;; ++counter) {
        if (const auto id = catalog::stableGameId(library, entry.system, entry.key.key(), counter)) {
            return *id;
        }
    }
}

catalog::StoredIdentity identityOf(const catalog::LibraryId& library, const catalog::IndexedEntry& indexed) {
    catalog::StoredIdentity identity;
    identity.id = indexed.id;
    identity.library = library;
    identity.system = indexed.entry.system;
    identity.key = indexed.entry.key;
    identity.keyVersion = catalog::currentLookupKeyVersion();
    identity.present = indexed.entry.file.has_value();
    identity.file = indexed.entry.file;
    return identity;
}

/// Writes the library and the systems, then the entries `entry(i)` gives for i in [0, count), in one transaction.
/// Writes the library, the systems and `count` entries (entryAt(i)) in one import transaction of `store`; with `bulk`,
/// the connection the store writes on, the search index is suspended for the writes and rebuilt before the commit.
template <class EntryAt>
Status writeEntries(catalog::ICatalogStore& store, const catalog::LibraryProfile& library,
                    const std::vector<catalog::SystemInfo>& systems, std::size_t count, EntryAt entryAt,
                    catalog_db::Connection* bulk = nullptr) {
    auto transaction = store.beginImport(library.id);
    if (!transaction) {
        return propagate(transaction.error());
    }
    if (bulk != nullptr) {
        if (auto suspended = catalog_db::suspendSearchIndex(*bulk); !suspended) {
            return suspended;
        }
    }
    if (auto put = (*transaction)->putLibrary(library); !put) {
        return put;
    }
    for (const catalog::SystemInfo& system : systems) {
        if (auto put = (*transaction)->putSystem(library.id, system); !put) {
            return put;
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        const catalog::IndexedEntry indexed = entryAt(i);
        if (auto put = (*transaction)->putIdentity(identityOf(library.id, indexed)); !put) {
            return put;
        }
        if (auto put = (*transaction)->putEntry(indexed); !put) {
            return put;
        }
    }
    if (bulk != nullptr) {
        if (auto resumed = catalog_db::resumeSearchIndex(*bulk); !resumed) {
            return resumed;
        }
    }
    return (*transaction)->commit();
}

/// The entries' GameIds with their indexes, in ascending GameId order. Errors: two entries with one GameId.
Expected<std::vector<std::pair<catalog::GameId, std::size_t>>> idOrder(const CatalogGeneratorOptions& options) {
    std::vector<std::pair<catalog::GameId, std::size_t>> order;
    order.reserve(options.entries);
    for (std::size_t i = 0; i < options.entries; ++i) {
        order.emplace_back(generatedEntry(options, i).id, i);
    }
    std::sort(order.begin(), order.end());
    for (std::size_t i = 1; i < order.size(); ++i) {
        if (order[i].first == order[i - 1].first) {
            return makeError(ErrorCode::InvalidArgument,
                             fmt::format("generated entries {} and {} have one GameId: choose another seed",
                                         order[i - 1].second, order[i].second));
        }
    }
    return order;
}

} // namespace

catalog::LibraryProfile generatedLibrary(const CatalogGeneratorOptions& options) {
    catalog::LibraryProfile library;
    library.id = catalog::LibraryId::parse(options.library).value();
    library.esdeHome = core::pathFromUtf8("C:/Users/Giocatore/ES-DE");
    library.installFolder = core::pathFromUtf8("C:/Program Files/ES-DE");
    library.romRoot = core::pathFromUtf8("E:/Giochi/roms");
    return library;
}

std::vector<catalog::SystemInfo> generatedSystems() {
    std::vector<catalog::SystemInfo> systems;
    for (std::size_t i = 0; i < kSystems.size(); ++i) {
        const GeneratedSystem& generated = kSystems[i];
        catalog::SystemInfo system;
        system.name = catalog::SystemName::parse(generated.name).value();
        system.fullName = std::string(generated.fullName);
        system.platforms = {std::string(generated.name)};
        system.theme = std::string(generated.name);
        system.romFolderConfigured = fmt::format("%ROMPATH%\\{}", generated.name);
        system.romFolder = core::pathFromUtf8(fmt::format("E:/Giochi/roms/{}", generated.name));
        system.gamelist = catalog::GamelistState::Read;
        if (i == kArchiveSystem) {
            system.availability = Availability::SourceUnavailable;
            system.unavailableReason = catalog::SourceUnavailableReason::FolderMissing;
        }
        systems.push_back(std::move(system));
    }
    return systems;
}

catalog::IndexedEntry generatedEntry(const CatalogGeneratorOptions& options, std::size_t index) {
    Random random = randomFor(options, 1, index);
    std::size_t systemIndex = pickSystem(random);
    const unsigned stateRoll = static_cast<unsigned>(random.below(100));
    EntryState state = EntryState::Game;
    if (stateRoll >= 84 && stateRoll < 88) {
        state = EntryState::MissingFile;
    } else if (stateRoll >= 88 && stateRoll < 91) {
        state = EntryState::NotAGame;
    } else if (stateRoll >= 91 && stateRoll < 95) {
        state = EntryState::DiscMember;
        systemIndex = kDiscSystem;
    } else if (stateRoll >= 95 && stateRoll < 97) {
        state = EntryState::Folder;
    }
    if (systemIndex == kArchiveSystem) {
        state = EntryState::Game; // a folder that cannot be listed: only what its gamelist says
    }
    const GeneratedSystem& system = kSystems[systemIndex];
    // Works chosen among about two thirds as many as entries: many entries share theirs (namesakes).
    const std::size_t workCount = std::max<std::size_t>(1, options.entries * 2 / 3);
    const Work work = makeWork(options, random.below(workCount));
    // Only a gamelist names the entries of a folder that could not be listed, a missing file or a folder.
    const bool scraped = state == EntryState::MissingFile || state == EntryState::Folder ||
                         systemIndex == kArchiveSystem || random.chance(85);
    const std::string title = random.chance(5) ? decomposed(work.title) : work.title;

    catalog::IndexedEntry indexed;
    catalog::CatalogEntry& entry = indexed.entry;
    entry.system = catalog::SystemName::parse(system.name).value();
    entry.state = state;
    const std::string region(random.pick(kRegions));
    const std::string collection = random.chance(15) ? fmt::format("Collection {}/", random.below(5) + 1) : "";
    std::string relativePath;
    switch (state) {
    case EntryState::NotAGame: {
        static constexpr std::array<catalog::NotAGameReason, 4> kReasons{
            catalog::NotAGameReason::ExtensionNotConfigured, catalog::NotAGameReason::NoLoadTxt,
            catalog::NotAGameReason::BiosOrDeviceSet, catalog::NotAGameReason::ExcludedBySetting};
        entry.notAGameReason = kReasons[index % kReasons.size()];
        const bool otherExtension = entry.notAGameReason == catalog::NotAGameReason::ExtensionNotConfigured;
        relativePath = fmt::format("{}{} ({}) [{}]{}", collection, stemOf(title), region, index,
                                   otherExtension ? ".bak" : system.extension);
        if (entry.notAGameReason == catalog::NotAGameReason::ExcludedBySetting) {
            entry.excludingSetting = "ShowHiddenFiles";
        }
        break;
    }
    case EntryState::DiscMember: {
        const std::string set = fmt::format("Disc Set {}/{} ({}) [{}]", index % 97, stemOf(title), region, index);
        relativePath = set + " (Track 2).bin";
        entry.discHead = catalog::makeLookupKey(set + ".cue").value();
        break;
    }
    case EntryState::Folder:
        relativePath = fmt::format("{}{} [{}]", collection, stemOf(title), index);
        break;
    default:
        relativePath = fmt::format("{}{} ({}) [{}]{}", collection, stemOf(title), region, index, system.extension);
        break;
    }
    entry.key = catalog::makeLookupKey(relativePath).value();
    if (systemIndex == kArchiveSystem) {
        entry.availability = Availability::SourceUnavailable;
        entry.unavailableReason = catalog::SourceUnavailableReason::FolderMissing;
    } else if (state == EntryState::MissingFile) {
        entry.availability = Availability::Missing;
    } else {
        const bool directory = state == EntryState::Folder;
        entry.file =
            catalog::FileFacts{directory, directory ? 0 : 65'536 + random.below(8'000'000),
                               130'000'000'000'000'000 + static_cast<std::int64_t>(random.below(1'000'000'000)),
                               entry.notAGameReason == catalog::NotAGameReason::ExcludedBySetting};
    }

    if (scraped) {
        entry.gamelistPath = "./" + relativePath;
        entry.title = title;
        catalog::GameMetadata& metadata = entry.metadata;
        metadata.name = random.chance(10) ? title + " \n" : title;
        metadata.sortName = work.sortName;
        metadata.description = fmt::format("An invented game of the generated catalog, a {} {} tale. {}.",
                                           random.pick(kAdjectives), random.pick(kNouns), kDescriptionOnlyWord);
        metadata.developer = std::string(random.pick(kCompanies));
        metadata.publisher = random.chance(70) ? std::string(random.pick(kCompanies)) : metadata.developer;
        metadata.genre = std::string(random.pick(kGenres));
        if (random.chance(90)) {
            metadata.releaseDate =
                catalog::ReleaseDate{1978 + static_cast<int>(random.below(31)), 1 + static_cast<int>(random.below(12)),
                                     1 + static_cast<int>(random.below(28))};
            const catalog::ReleaseDate& date = *metadata.releaseDate;
            entry.raw.originalForms.push_back(
                {"releasedate", fmt::format("{:04}{:02}{:02}T000000", date.year, date.month, date.day)});
        }
        if (random.chance(70)) {
            metadata.rating = catalog::Rating{static_cast<int>(random.below(11))};
            entry.raw.originalForms.push_back({"rating", catalog::formatRating(*metadata.rating)});
        }
        if (random.chance(80)) {
            static constexpr std::array<catalog::Players, 4> kPlayers{{{1, 1}, {1, 2}, {1, 4}, {2, std::nullopt}}};
            metadata.players = random.pick(kPlayers);
            entry.raw.originalForms.push_back({"players", catalog::formatPlayers(*metadata.players)});
        }
        if (random.chance(5)) {
            entry.raw.unknownElements.push_back({"region", region});
        }
        entry.flags.favorite = random.chance(5);
        entry.flags.playCount = random.chance(20) ? random.below(50) : 0;
    } else {
        entry.title = catalog::titleFromFileStem(relativePath, state == EntryState::Folder);
    }

    if (random.chance(70)) {
        indexed.media[catalog::MediaKind::Cover] = catalog::MediaPresence{true, ".png"};
    }
    if (random.chance(30)) {
        indexed.media[catalog::MediaKind::Video] = catalog::MediaPresence{true, ".mp4"};
    }
    indexed.sortKey = catalog::sortKeyOf(entry).value();
    indexed.id = gameIdOf(catalog::LibraryId::parse(options.library).value(), entry);
    return indexed;
}

Expected<GeneratedCatalog> generateCatalog(const CatalogGeneratorOptions& options) {
    auto order = idOrder(options);
    if (!order) {
        return propagate(order.error());
    }
    GeneratedCatalog catalog;
    catalog.library = generatedLibrary(options);
    catalog.systems = generatedSystems();
    catalog.entries.reserve(order->size());
    for (const auto& [id, index] : *order) {
        catalog.entries.push_back(generatedEntry(options, index));
    }
    catalog.entryCount = catalog.entries.size();
    return catalog;
}

Status writeCatalog(catalog::ICatalogStore& store, const GeneratedCatalog& catalog) {
    return writeEntries(store, catalog.library, catalog.systems, catalog.entries.size(),
                        [&](std::size_t i) { return catalog.entries[i]; });
}

Expected<GeneratedCatalog> generateCatalogDatabase(const std::filesystem::path& file,
                                                   const CatalogGeneratorOptions& options, bool keepEntries) {
    GeneratedCatalog catalog;
    std::vector<std::pair<catalog::GameId, std::size_t>> order;
    if (keepEntries) {
        auto generated = generateCatalog(options);
        if (!generated) {
            return propagate(generated.error());
        }
        catalog = std::move(*generated);
    } else {
        auto ids = idOrder(options);
        if (!ids) {
            return propagate(ids.error());
        }
        order = std::move(*ids);
        catalog.library = generatedLibrary(options);
        catalog.systems = generatedSystems();
        catalog.entryCount = order.size();
    }
    auto database = catalog_db::openCatalogDatabase(file);
    if (!database) {
        return propagate(database.error());
    }
    auto store = catalog_db::SqliteCatalogStore::open(*database);
    if (!store) {
        return propagate(store.error());
    }
    catalog_db::Connection* bulk = options.suspendSearchIndex ? &database->writer() : nullptr;
    const Status written = keepEntries
                               ? writeEntries(
                                     **store, catalog.library, catalog.systems, catalog.entries.size(),
                                     [&](std::size_t i) { return catalog.entries[i]; }, bulk)
                               : writeEntries(
                                     **store, catalog.library, catalog.systems, order.size(),
                                     [&](std::size_t i) { return generatedEntry(options, order[i].second); }, bulk);
    if (!written) {
        return propagate(written.error());
    }
    return catalog;
}

std::string generatedComposed(std::string_view text) {
    return replacedLetters(text, false);
}

std::vector<std::string> generatedWords(std::string_view text) {
    // The accented letters the generator writes, each with the letter unicode61 folds it to.
    static constexpr std::array<std::pair<std::string_view, char>, 40> kFolds{{
        {"é", 'e'}, {"É", 'e'}, {"è", 'e'}, {"ê", 'e'}, {"ë", 'e'}, {"à", 'a'}, {"â", 'a'}, {"ä", 'a'},
        {"á", 'a'}, {"å", 'a'}, {"Å", 'a'}, {"ç", 'c'}, {"ñ", 'n'}, {"ü", 'u'}, {"Ü", 'u'}, {"ö", 'o'},
        {"ô", 'o'}, {"ō", 'o'}, {"û", 'u'}, {"î", 'i'}, {"ï", 'i'}, {"í", 'i'}, {"ó", 'o'}, {"ú", 'u'},
        {"ù", 'u'}, {"ã", 'a'}, {"õ", 'o'}, {"ÿ", 'y'}, {"ò", 'o'}, {"ì", 'i'}, {"Ç", 'c'}, {"Ñ", 'n'},
        {"Ö", 'o'}, {"Ä", 'a'}, {"È", 'e'}, {"Ê", 'e'}, {"Ô", 'o'}, {"Û", 'u'}, {"Î", 'i'}, {"Á", 'a'},
    }};
    std::vector<std::string> words;
    std::string word;
    const auto endWord = [&] {
        if (!word.empty()) {
            words.push_back(std::move(word));
            word.clear();
        }
    };
    std::size_t at = 0;
    while (at < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[at]);
        if (c < 0x80) {
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                word += static_cast<char>(c);
            } else if (c >= 'A' && c <= 'Z') {
                word += static_cast<char>(c - 'A' + 'a');
            } else {
                endWord();
            }
            ++at;
            continue;
        }
        // Combining marks U+0300 to U+036F (CC 80 to CD AF) continue a word and fold to nothing.
        if ((c == 0xCC || (c == 0xCD && at + 1 < text.size() && static_cast<unsigned char>(text[at + 1]) <= 0xAF)) &&
            at + 1 < text.size()) {
            at += 2;
            continue;
        }
        bool folded = false;
        for (const auto& [letter, base] : kFolds) {
            if (text.substr(at).starts_with(letter)) {
                word += base;
                at += letter.size();
                folded = true;
                break;
            }
        }
        if (!folded) {
            // Not a letter the generator writes: a separator here (the oracle is only meant for generated text).
            endWord();
            ++at;
            while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
                ++at;
            }
        }
    }
    endWord();
    return words;
}

} // namespace rv::test
