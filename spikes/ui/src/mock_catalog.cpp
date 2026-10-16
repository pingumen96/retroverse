#include "mock_catalog.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>

#include <fmt/format.h>

namespace rv::spike_ui {
namespace {

/// SplitMix64 (Steele, Lea and Flood): a fixed seed gives the same games in every run (CLAUDE.md: determinism).
class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) noexcept : m_state(seed) {}

    std::uint64_t next() noexcept { return mix(m_state += 0x9E3779B97F4A7C15ull); }
    /// A value in [0, bound), bound > 0 (the modulo bias is irrelevant here).
    std::size_t below(std::size_t bound) noexcept { return static_cast<std::size_t>(next() % bound); }

    [[nodiscard]] static std::uint64_t mix(std::uint64_t value) noexcept {
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
        return value ^ (value >> 31);
    }

private:
    std::uint64_t m_state;
};

constexpr std::uint64_t kSeed = 0x5265747256657273ull; // "RetrVers"

// Invented names only (style §17.2): no title, studio or platform of the real world.
constexpr std::array<std::string_view, 48> kNouns{
    "Lanterna", "Corsa",      "Faro",     "Giardino", "Cavalieri", "Notte",     "Rally",    "Astronomo",
    "Mare",     "Tempesta",   "Torre",    "Volo",     "Bottega",   "Lupi",      "Città",    "Postino",
    "Regata",   "Biblioteca", "Galassia", "Castello", "Isola",     "Fiume",     "Treno",    "Orologio",
    "Specchio", "Labirinto",  "Vulcano",  "Ponte",    "Miniera",   "Foresta",   "Deserto",  "Ghiacciaio",
    "Pianeta",  "Cometa",     "Drago",    "Automa",   "Corsari",   "Guardiani", "Viaggio",  "Leggenda",
    "Cronache", "Sentiero",   "Officina", "Mercato",  "Giostra",   "Arena",     "Fortezza", "Carillon"};
constexpr std::array<std::string_view, 40> kComplements{
    "di Vetro",      "del Vapore",     "sul Lungomare",  "delle Stelle",    "di Bit",     "sul Ring",   "degli Orologi",
    "di Ghiaccio",   "di Neon",        "del Vento",      "a Mosaico",       "di Sabbia",  "d'Argento",  "di Cristallo",
    "del Tramonto",  "di Ferro",       "delle Nebbie",   "del Nord",        "del Sud",    "di Fuoco",   "di Carta",
    "della Luna",    "del Sole",       "d'Oro",          "dei Ricordi",     "senza Fine", "in Fiamme",  "del Drago",
    "dei Corsari",   "di Mezzogiorno", "delle Dolomiti", "dell'Alchimista", "nel Bosco",  "sotto Zero", "in Orbita",
    "di Porcellana", "a Vapore",       "di Velluto",     "fra le Nuvole",   "del Faro"};
constexpr std::array<std::string_view, 16> kSuffixes{
    "", "", "", "", "", "", "II", "III", "IV", "Deluxe", "Turbo", "Special", "Gold", "Remix", "Origini", "Il Ritorno"};
constexpr std::array<std::string_view, 12> kPlatforms{"8 bit",
                                                      "16 bit",
                                                      "32 bit",
                                                      "64 bit",
                                                      "Arcade",
                                                      "Portatile",
                                                      "Portatile a colori",
                                                      "Computer a 8 bit",
                                                      "Computer a 16 bit",
                                                      "Console a CD",
                                                      "Vettoriale",
                                                      "LCD tascabile"};
constexpr std::array<std::string_view, 30> kStudios{
    "Studio Aurora",      "Officina Pixel",   "Officina Lunare",    "Serra Digitale",  "Ferro e Fumo",
    "Studio Ingranaggio", "Curva Stretta",    "Lente Blu",          "Pugno Dorato",    "Ala Blu",
    "Nord Software",      "Bit Nebbiosi",     "Rosa dei Venti",     "Studio Faro",     "Laboratorio Cometa",
    "Vapore Giochi",      "Studio Corallo",   "Quarzo Interattivo", "Fucina Ludica",   "Studio Alba",
    "Pixel Sereno",       "Onda Lunga",       "Vetro Soffiato",     "Studio Ciliegio", "Polvere di Stelle",
    "Mulino Software",    "Studio Ventaglio", "Giochi del Golfo",   "Studio Brezza",   "Cantiere Ludico"};
constexpr std::array<std::string_view, 12> kGenres{"Sparatutto",     "Corse",       "Avventura", "Rompicapo",
                                                   "Picchiaduro",    "Piattaforme", "Sport",     "Simulazione",
                                                   "Gioco di ruolo", "Azione",      "Strategia", "Musicale"};
constexpr std::array<std::string_view, 4> kPlayers{"1", "1-2", "1-4", "2"};
constexpr std::array<std::string_view, 12> kOpenings{"Un viaggio tra stanze dipinte a mano e segreti da scoprire.",
                                                     "Una sfida a tempo in cui ogni secondo conta.",
                                                     "Mappe che cambiano a ogni partita e nemici sempre diversi.",
                                                     "Enigmi di luce e ingranaggi da rimettere in moto.",
                                                     "Una corsa contro il tramonto lungo strade tortuose.",
                                                     "Un mondo sospeso tra le nuvole, da esplorare in volo.",
                                                     "Una città addormentata da risvegliare un quartiere alla volta.",
                                                     "Duelli rapidi in arene piene di trappole.",
                                                     "Una spedizione nel ghiaccio con provviste contate.",
                                                     "Una bottega da mandare avanti tra clienti e debiti.",
                                                     "Melodie da seguire a ritmo su piste luminose.",
                                                     "Strategie a turni su isole contese."};
constexpr std::array<std::string_view, 10> kClosings{"Il secondo giocatore può unirsi in qualsiasi momento.",
                                                     "Ogni livello nasconde una scorciatoia.",
                                                     "Un diario raccoglie ciò che si è scoperto.",
                                                     "La colonna sonora cambia con il meteo.",
                                                     "Le partite brevi si giocano in pochi minuti.",
                                                     "Un editor permette di disegnare nuovi percorsi.",
                                                     "I segreti sbloccano un finale alternativo.",
                                                     "Le classifiche ricordano i tempi migliori.",
                                                     "Un tutorial accompagna i primi passi.",
                                                     "Nel finale si torna al punto di partenza, cambiati."};

[[nodiscard]] std::string lowerAscii(std::string_view text) {
    std::string lower(text);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return lower;
}

/// The GameId text form of a hash of `index`: 'g', 16 lowercase hexadecimal digits, top bit clear, never 0.
[[nodiscard]] std::string gameId(std::size_t index) {
    std::uint64_t value = SplitMix64::mix(kSeed ^ (static_cast<std::uint64_t>(index) * 0x2545F4914F6CDD1Dull));
    value &= 0x7FFF'FFFF'FFFF'FFFFull;
    if (value == 0) {
        value = 1;
    }
    return fmt::format("g{:016x}", value);
}

} // namespace

std::vector<ui::CatalogEntry> mockCatalog(std::size_t count) {
    count = std::max<std::size_t>(count, 1);
    SplitMix64 random(kSeed);
    std::vector<ui::CatalogEntry> games(count);
    for (std::size_t index = 0; index < count; ++index) {
        ui::CatalogEntry& game = games[index];
        game.id = gameId(index);
        const std::string_view suffix = kSuffixes[random.below(kSuffixes.size())];
        game.title = std::string(kNouns[random.below(kNouns.size())]) + " " +
                     std::string(kComplements[random.below(kComplements.size())]);
        if (!suffix.empty()) {
            game.title += " ";
            game.title += suffix;
        }
        game.platform = kPlatforms[random.below(kPlatforms.size())];
        game.year = 1977 + static_cast<int>(random.below(30));
        game.developer = kStudios[random.below(kStudios.size())];
        game.publisher =
            random.below(3) == 0 ? std::string(kStudios[random.below(kStudios.size())]) + " Edizioni" : game.developer;
        game.genre = kGenres[random.below(kGenres.size())];
        game.players = kPlayers[random.below(kPlayers.size())];
        game.description = std::string(kOpenings[random.below(kOpenings.size())]) + " " +
                           std::string(kClosings[random.below(kClosings.size())]);
        // A few games that cannot start, spread over the list: the labels and styles of CatalogEntry::state.
        if (index % 47 == 5) {
            game.state = "unavailable";
        } else if (index % 61 == 11) {
            game.state = "needs-configuration";
        }
    }
    // The catalog's order: the title folded, then the id (a total order: titles repeat across platforms).
    std::vector<std::pair<std::string, std::size_t>> keys;
    keys.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        keys.emplace_back(lowerAscii(games[index].title), index);
    }
    std::sort(keys.begin(), keys.end(), [&games](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : games[a.second].id < games[b.second].id;
    });
    std::vector<ui::CatalogEntry> sorted;
    sorted.reserve(count);
    for (const auto& [key, index] : keys) {
        sorted.push_back(std::move(games[index]));
    }
    // The marker in the middle: found alone by kMarkerWord, on the first platform chip's platform, ready to start.
    ui::CatalogEntry& marker = sorted[count / 2];
    marker.developer = "Studio Mezzanotte";
    marker.publisher = "Edizioni Mezzanotte";
    marker.platform = sorted.front().platform;
    marker.state.clear();
    return sorted;
}

} // namespace rv::spike_ui
