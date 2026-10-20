#include "support/fixture_collection.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/core/random.hpp>
#include <rv/core/text.hpp>

namespace rv::test {
namespace {

constexpr std::size_t kMaxTitles = 20'000;
constexpr std::uint64_t kMaxRomBytes = 64ull * 1024ull * 1024ull;

// Gentle, ordinary words, chosen so that their combinations name no published game.
constexpr std::array<std::string_view, 32> kAdjectives{
    "Amber",    "Brambly", "Copper",  "Dappled", "Ember",  "Fennel", "Gilded",  "Hazel",
    "Indigo",   "Juniper", "Lilac",   "Mossy",   "Nutmeg", "Opal",   "Pewter",  "Quilted",
    "Russet",   "Saffron", "Thistle", "Umber",   "Velvet", "Willow", "Woolly",  "Yarrow",
    "Marigold", "Cobalt",  "Clover",  "Tawny",   "Briny",  "Sorrel", "Heather", "Linen"};
constexpr std::array<std::string_view, 40> kNouns{
    "Lantern",  "Orchard",  "Thimble",  "Teapot",   "Acorn",     "Burrow",   "Compass",  "Dovecote",
    "Kettle",   "Ferry",    "Garret",   "Hedgerow", "Inkwell",   "Jamboree", "Kiln",     "Loom",
    "Meadow",   "Nook",     "Oriel",    "Parasol",  "Quarry",    "Rookery",  "Satchel",  "Trellis",
    "Umbrella", "Vineyard", "Windmill", "Abacus",   "Bellows",   "Carousel", "Dumpling", "Easel",
    "Fiddle",   "Gazebo",   "Hammock",  "Igloo",    "Marmalade", "Pinwheel", "Toboggan", "Weathervane"};
constexpr std::array<std::string_view, 14> kTails{"Almanac", "Errands",  "Expedition", "Festival",  "Mystery",
                                                  "Rally",   "Workshop", "Parade",     "Picnic",    "Regatta",
                                                  "Caper",   "Jubilee",  "Lullaby",    "Tournament"};
constexpr std::array<std::string_view, 3> kNumerals{"II", "III", "IV"};
constexpr std::array<std::string_view, 8> kDevelopers{"Bramble Bits",    "Lumen Forge",    "Kettle Interactive",
                                                      "Northwind Pixel", "Moss & Tallow",  "Little Comet",
                                                      "Paper Owl",       "Saltmarsh Games"};
constexpr std::array<std::string_view, 5> kPublishers{"Harborlight", "Tinplate Media", "Orchard House",
                                                      "Bluebell Arcade", "Quiet Fox"};
constexpr std::array<std::string_view, 9> kGenres{"Azione",      "Avventura", "Puzzle",    "Corsa, Guida", "Sport",
                                                  "Piattaforme", "RPG",       "Strategia", "Sparatutto"};
constexpr std::array<std::string_view, 3> kPlayers{"1", "1-2", "1-4"};
constexpr std::array<std::string_view, 6> kPlots{"a map, a lantern and a long walk home",
                                                 "a village fair that must open by noon",
                                                 "a lighthouse keeper's lost cat",
                                                 "a river race on borrowed rafts",
                                                 "a puzzle box that rearranges the countryside",
                                                 "an orchard whose trees walk at night"};

template <class T, std::size_t N>
std::string_view pick(core::Random& random, const std::array<T, N>& items) {
    return items[static_cast<std::size_t>(random.below(N))];
}

std::string inventTitle(core::Random& random) {
    const std::uint64_t form = random.below(100);
    const std::string_view adjective = pick(random, kAdjectives);
    const std::string_view noun = pick(random, kNouns);
    if (form < 40) {
        return fmt::format("{} {}", adjective, noun);
    }
    if (form < 70) {
        return fmt::format("{} {} {}", adjective, noun, pick(random, kTails));
    }
    if (form < 82) {
        return fmt::format("{}: The {} {}", noun, adjective, pick(random, kTails));
    }
    if (form < 92) {
        return fmt::format("{} {} {}", adjective, noun, pick(random, kNumerals));
    }
    const std::string_view second = pick(random, kAdjectives);
    return fmt::format("The {} {} and the {} {}", adjective, noun, second, pick(random, kNouns));
}

std::string stemOf(std::string_view title, std::string_view tags) {
    std::string stem(title);
    for (std::size_t at = stem.find(": "); at != std::string::npos; at = stem.find(": ", at + 3)) {
        stem.replace(at, 2, " - ");
    }
    return stem + " " + std::string(tags);
}

std::string xmlEscaped(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

/// ES-DE's rating text: 0 to 1 in steps of 0.05, without trailing zeros ("0.65", "0.8", "1").
std::string ratingText(std::uint64_t twentieths) {
    if (twentieths >= 20) {
        return "1";
    }
    std::string text = fmt::format("0.{:02}", twentieths * 5);
    while (text.back() == '0') {
        text.pop_back();
    }
    return text.back() == '.' ? text + "0" : text;
}

std::string regionOf(std::size_t index) {
    if (index % 20 == 7) {
        return "(World)";
    }
    if (index % 20 == 13) {
        return "(Europe)";
    }
    if (index % 40 == 19) {
        return "(Japan)";
    }
    return "(USA)";
}

bool takesDuplicate(std::size_t index) {
    const std::size_t residue = index % 20;
    return residue == 1 || residue == 4 || residue == 9;
}

Expected<std::uint64_t> unsignedKey(const nlohmann::json& value, std::string_view key, std::string_view context) {
    if (!value.contains(key) || !value.at(std::string(key)).is_number_unsigned()) {
        return makeError(ErrorCode::ValidationFailed,
                         fmt::format("{}: \"{}\" is required, an integer of 0 or more", context, key));
    }
    return value.at(std::string(key)).get<std::uint64_t>();
}

Expected<std::string> pathKey(const nlohmann::json& value, std::string_view key, std::string_view context) {
    if (!value.contains(key) || !value.at(std::string(key)).is_string() ||
        value.at(std::string(key)).get<std::string>().empty()) {
        return makeError(ErrorCode::ValidationFailed,
                         fmt::format("{}: \"{}\" is required, a non-empty string", context, key));
    }
    return value.at(std::string(key)).get<std::string>();
}

} // namespace

Expected<CollectionSpec> parseCollectionSpec(const nlohmann::json& value, std::string_view context) {
    const auto fail = [&](std::string message) {
        return makeError(ErrorCode::ValidationFailed, fmt::format("{}: {}", context, message));
    };
    if (!value.is_object()) {
        return fail("a collection must be an object");
    }
    static constexpr std::array<std::string_view, 12> kKeys{
        "seed",     "titles",      "duplicates", "videos", "romFolder", "romExtension",
        "romSize",  "gamelist",    "mediaFolder", "coverSizes", "covers", "from"};
    for (const auto& [key, item] : value.items()) {
        if (std::find(kKeys.begin(), kKeys.end(), key) == kKeys.end()) {
            return fail("unknown key \"" + key +
                        "\" of a collection (seed, titles, duplicates, videos, romFolder, romExtension, romSize, "
                        "gamelist, mediaFolder, coverSizes, covers, from)");
        }
    }
    std::uint64_t seed = 0;
    std::uint64_t titles = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t videos = 0;
    std::uint64_t romSize = 0;
    for (const auto& [key, target] :
         {std::pair{"seed", &seed}, std::pair{"titles", &titles}, std::pair{"duplicates", &duplicates},
          std::pair{"videos", &videos}, std::pair{"romSize", &romSize}}) {
        auto number = unsignedKey(value, key, context);
        if (!number) {
            return propagate(number.error());
        }
        *target = *number;
    }
    CollectionSpec spec;
    for (const auto& [key, target] :
         {std::pair{"romFolder", &spec.romFolder}, std::pair{"romExtension", &spec.romExtension},
          std::pair{"gamelist", &spec.gamelist}, std::pair{"mediaFolder", &spec.mediaFolder}}) {
        auto text = pathKey(value, key, context);
        if (!text) {
            return propagate(text.error());
        }
        *target = std::move(*text);
    }
    if (titles > kMaxTitles) {
        return fail(fmt::format("\"titles\" must be at most {}", kMaxTitles));
    }
    std::size_t room = 0;
    for (std::size_t i = 0; i < titles; ++i) {
        room += takesDuplicate(i) ? 1 : 0;
    }
    if (duplicates > room) {
        return fail(fmt::format("\"duplicates\" must be at most {} for {} titles (3 per 20)", room, titles));
    }
    if (videos > titles) {
        return fail("\"videos\" must be at most \"titles\"");
    }
    if (romSize > kMaxRomBytes) {
        return fail("\"romSize\" must be at most 64 MiB");
    }
    if (value.contains("coverSizes")) {
        const nlohmann::json& sizes = value.at("coverSizes");
        if (!sizes.is_array() || sizes.empty()) {
            return fail("\"coverSizes\" must be a non-empty array of [width, height]");
        }
        for (const nlohmann::json& size : sizes) {
            const bool valid = size.is_array() && size.size() == 2 && size[0].is_number_unsigned() &&
                               size[1].is_number_unsigned() && size[0].get<std::uint64_t>() >= 1 &&
                               size[0].get<std::uint64_t>() <= 4096 && size[1].get<std::uint64_t>() >= 1 &&
                               size[1].get<std::uint64_t>() <= 4096;
            if (!valid) {
                return fail("\"coverSizes\" must be a non-empty array of [width, height], each 1 to 4096");
            }
            spec.coverSizes.emplace_back(size[0].get<int>(), size[1].get<int>());
        }
    }
    for (const auto& [key, target] : {std::pair{"covers", static_cast<std::size_t*>(nullptr)},
                                      std::pair{"from", &spec.from}}) {
        if (!value.contains(key)) {
            continue;
        }
        auto number = unsignedKey(value, key, context);
        if (!number) {
            return propagate(number.error());
        }
        if (*number > titles) {
            return fail(fmt::format("\"{}\" must be at most \"titles\"", key));
        }
        if (target != nullptr) {
            *target = static_cast<std::size_t>(*number);
        } else {
            spec.covers = static_cast<std::size_t>(*number);
        }
    }
    if (!spec.romExtension.starts_with('.') || spec.romExtension.find_first_of("/\\") != std::string::npos) {
        return fail("\"romExtension\" must be an extension such as \".sfc\"");
    }
    spec.seed = seed;
    spec.titles = static_cast<std::size_t>(titles);
    spec.duplicates = static_cast<std::size_t>(duplicates);
    spec.videos = static_cast<std::size_t>(videos);
    spec.romSize = romSize;
    return spec;
}

GeneratedCollection generateCollection(const CollectionSpec& spec) {
    GeneratedCollection collection;
    core::Random random(spec.seed);
    std::set<std::string> keys;
    std::size_t duplicatesLeft = spec.duplicates;
    std::string& xml = collection.gamelist;
    xml = "<?xml version=\"1.0\"?>\n<gameList>\n";

    for (std::size_t index = 0; index < spec.titles; ++index) {
        std::string title;
        do {
            title = inventTitle(random);
        } while (!keys.insert(core::toLowerAscii(title)).second);

        PicturePayload cover;
        cover.title = title;
        cover.seed = static_cast<std::uint32_t>((spec.seed + index * 7919u) & 0xFFFFFFFFu);
        cover.quickPng = true;
        std::string coverExtension = ".png";
        if (index % 10 == 3) {
            cover.format = PictureFormat::Jpeg;
            coverExtension = ".jpg";
        }
        if (!spec.coverSizes.empty()) {
            const auto& [width, height] = spec.coverSizes[index % spec.coverSizes.size()];
            cover.width = width;
            cover.height = height;
        } else if (index % 50 == 11) {
            std::swap(cover.width, cover.height);
        }
        const bool hasCover = !spec.covers || index < *spec.covers;

        const auto addRelease = [&](std::string tags, bool representative) {
            CollectionGame game;
            game.titleIndex = index;
            game.title = title;
            game.stem = stemOf(title, tags);
            game.tags = std::move(tags);
            game.representative = representative;
            game.romSeed = spec.seed * 1'000'003u + collection.games.size();
            game.cover = cover;
            game.coverExtension = coverExtension;
            game.hasCover = hasCover;
            if (representative) {
                game.favorite = index % 25 == 0;
                game.playCount = random.below(4) == 0 ? static_cast<std::uint32_t>(random.between(1, 60)) : 0u;
                game.hasVideo = hasCover && index < spec.videos;
                game.video.spec.width = 320;
                game.video.spec.height = 240;
                game.video.spec.seed = cover.seed;
            }

            const std::uint64_t year = 1990 + random.below(9);
            const std::uint64_t month = 1 + random.below(12);
            const std::uint64_t day = 1 + random.below(28);
            xml += "\t<game>\n";
            xml += fmt::format("\t\t<path>./{}{}</path>\n", xmlEscaped(game.stem), xmlEscaped(spec.romExtension));
            xml += fmt::format("\t\t<name>{}</name>\n", xmlEscaped(title));
            xml += fmt::format("\t\t<desc>{}: {}.</desc>\n", xmlEscaped(title), pick(random, kPlots));
            xml += fmt::format("\t\t<rating>{}</rating>\n", ratingText(random.below(21)));
            xml += fmt::format("\t\t<releasedate>{:04}{:02}{:02}T000000</releasedate>\n", year, month, day);
            xml += fmt::format("\t\t<developer>{}</developer>\n", xmlEscaped(pick(random, kDevelopers)));
            xml += fmt::format("\t\t<publisher>{}</publisher>\n", xmlEscaped(pick(random, kPublishers)));
            xml += fmt::format("\t\t<genre>{}</genre>\n", xmlEscaped(pick(random, kGenres)));
            xml += fmt::format("\t\t<players>{}</players>\n", pick(random, kPlayers));
            if (game.favorite) {
                xml += "\t\t<favorite>true</favorite>\n";
            }
            if (game.playCount > 0) {
                xml += fmt::format("\t\t<playcount>{}</playcount>\n", game.playCount);
                xml += fmt::format("\t\t<lastplayed>2026{:02}{:02}T2000{:02}</lastplayed>\n", month, day,
                                   game.playCount % 60);
            }
            xml += "\t</game>\n";
            collection.games.push_back(std::move(game));
        };

        const bool duplicated = duplicatesLeft > 0 && takesDuplicate(index);
        if (duplicated && index % 20 == 9) {
            // A revision: the plain release first, then "Rev 1", the representative by No-Intro's rule.
            addRelease("(USA)", false);
            addRelease("(USA) (Rev 1)", true);
        } else {
            addRelease(regionOf(index), true);
            if (duplicated) {
                addRelease("(Europe)", false);
            }
        }
        if (duplicated) {
            --duplicatesLeft;
        }
    }
    xml += "</gameList>\n";
    return collection;
}

} // namespace rv::test
