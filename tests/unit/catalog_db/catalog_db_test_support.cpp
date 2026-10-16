#include "catalog_db_test_support.hpp"

#include <array>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include <fmt/format.h>

#include <rv/catalog/identity.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>

namespace rv::test {

std::filesystem::path databaseFileIn(const TempDir& dir) {
    return dir.path() / "data" / "retroverse.sqlite";
}

catalog::LibraryProfile testLibrary(std::string_view id) {
    catalog::LibraryProfile profile;
    profile.id = catalog::LibraryId::parse(id).value();
    profile.esdeHome = core::pathFromUtf8("C:/Users/Giocatore/ES-DE");
    profile.installFolder = core::pathFromUtf8("C:/Program Files/ES-DE");
    profile.mediaRoot = core::pathFromUtf8("E:/Média/downloaded_media");
    return profile;
}

catalog::SystemInfo testSystem(std::string_view name, const std::filesystem::path& romFolder) {
    catalog::SystemInfo system;
    system.name = catalog::SystemName::parse(name).value();
    system.fullName = fmt::format("Test System {}", name);
    system.platforms = {std::string(name), "test-platform"};
    system.theme = std::string(name);
    system.romFolderConfigured = fmt::format("%ROMPATH%\\{}", name);
    system.romFolder = romFolder;
    system.gamelist = catalog::GamelistState::Read;
    system.alternativeEmulator = "Fake Emulator (Standalone)";
    return system;
}

catalog::CatalogEntry fullEntry(std::string_view system, std::string_view relativePath, std::string_view title) {
    catalog::CatalogEntry entry;
    entry.system = catalog::SystemName::parse(system).value();
    entry.key = catalog::makeLookupKey(relativePath).value();
    entry.state = catalog::EntryState::Game;
    entry.file = catalog::FileFacts{false, 1'048'576, 133'000'000'000'000'000, false};
    entry.gamelistPath = "./" + std::string(relativePath);
    entry.title = std::string(title);
    catalog::GameMetadata& metadata = entry.metadata;
    metadata.name = std::string(title) + " \n";
    metadata.sortName = "Sort of " + std::string(title);
    metadata.description = "A line.\nAnother line, with a tab\tand \"quotes\".";
    metadata.developer = "Mirage Soft ";
    metadata.publisher = "Éditions Pixel";
    metadata.genre = "Corsa, Guida / Moto";
    metadata.releaseDate = catalog::ReleaseDate{1997, 7, 2};
    metadata.rating = catalog::Rating{7};
    metadata.players = catalog::Players{1, 4};
    entry.raw.unknownElements = {{"region", "eu"}, {"md5", "0123456789abcdef"}, {"region", "jp"}};
    entry.raw.originalForms = {{"releasedate", "19970702T000000"}, {"rating", "0.70"}, {"players", "1-4"}};
    entry.altEmulator = "Fake Emulator (Alternative)";
    entry.flags.favorite = true;
    entry.flags.kidGame = true;
    entry.flags.completed = true;
    entry.flags.playCount = 12;
    entry.flags.playTimeSeconds = 3'600;
    entry.flags.lastPlayed = catalog::LocalDateTime{2026, 10, 1, 21, 15, 30};
    return entry;
}

catalog::CatalogEntry bareEntry(std::string_view system, std::string_view relativePath) {
    catalog::CatalogEntry entry;
    entry.system = catalog::SystemName::parse(system).value();
    entry.key = catalog::makeLookupKey(relativePath).value();
    entry.state = catalog::EntryState::Game;
    entry.file = catalog::FileFacts{false, 65'536, 132'000'000'000'000'000, false};
    entry.title = catalog::titleFromFileStem(relativePath);
    return entry;
}

catalog::IndexedEntry indexedEntry(catalog::GameId id, catalog::CatalogEntry entry, bool withMedia) {
    catalog::IndexedEntry indexed;
    indexed.id = id;
    indexed.sortKey = catalog::sortKeyOf(entry).value();
    indexed.entry = std::move(entry);
    if (withMedia) {
        indexed.media[catalog::MediaKind::Cover] = catalog::MediaPresence{true, ".png"};
        indexed.media[catalog::MediaKind::Video] = catalog::MediaPresence{true, ".mp4"};
    }
    return indexed;
}

catalog::StoredIdentity identityOf(catalog::GameId id, const catalog::LibraryId& library,
                                   const catalog::CatalogEntry& entry) {
    catalog::StoredIdentity identity;
    identity.id = id;
    identity.library = library;
    identity.system = entry.system;
    identity.key = entry.key;
    identity.keyVersion = catalog::currentLookupKeyVersion();
    identity.present = entry.file.has_value();
    identity.file = entry.file;
    return identity;
}

catalog::GameId gameIdOf(const catalog::LibraryId& library, const catalog::CatalogEntry& entry, std::uint32_t counter) {
    return catalog::stableGameId(library, entry.system, entry.key.key(), counter).value();
}

std::vector<catalog_db::Migration> migrationsUpTo(int last) {
    std::vector<catalog_db::Migration> migrations = catalog_db::programMigrations();
    for (int version = catalog_db::programSchemaVersion() + 1; version <= last; ++version) {
        migrations.push_back(catalog_db::Migration{
            version, fmt::format("test v{}", version), [version](catalog_db::Connection& connection) {
                return connection.execute(fmt::format("CREATE TABLE test_v{} (x INTEGER) STRICT", version));
            }});
    }
    return migrations;
}

std::optional<DatabaseHeader> readDatabaseHeader(const std::filesystem::path& file) {
    std::ifstream stream(std::filesystem::path(core::extendedLengthPath(file)), std::ios::binary);
    std::array<unsigned char, 100> bytes{};
    if (!stream || !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return std::nullopt;
    }
    constexpr std::string_view kMagic("SQLite format 3\0", 16);
    if (std::string_view(reinterpret_cast<const char*>(bytes.data()), kMagic.size()) != kMagic) {
        return std::nullopt;
    }
    const auto bigEndian = [&](std::size_t at) {
        return (std::uint32_t{bytes[at]} << 24) | (std::uint32_t{bytes[at + 1]} << 16) |
               (std::uint32_t{bytes[at + 2]} << 8) | std::uint32_t{bytes[at + 3]};
    };
    DatabaseHeader header;
    header.writeVersion = bytes[18];
    header.userVersion = bigEndian(60);
    header.applicationId = static_cast<std::int32_t>(bigEndian(68));
    return header;
}

bool existsAnyLength(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(core::extendedLengthPath(path)), ec) && !ec;
}

} // namespace rv::test
