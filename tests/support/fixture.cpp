#include "support/fixture.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <set>
#include <system_error>
#include <vector>

#include <windows.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/core/case_fold.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/relative_path.hpp>
#include <rv/core/text.hpp>
#include <rv/core/time.hpp>
#include <rv/core/utf8.hpp>

#include "support/file_system.hpp"

namespace rv::test {
namespace {

constexpr std::string_view kRootToken = "{{FIXTURE_ROOT}}";
constexpr std::string_view kManifestName = "files.json";
constexpr std::uint64_t kMaxPayloadBytes = 64ull * 1024ull * 1024ull;

std::filesystem::path buildDir() {
    return core::pathFromUtf8(RV_TEST_SUPPORT_BUILD_DIR);
}

enum class TextKind { None, Xml, Json, Plain };

TextKind textKindOf(const std::filesystem::path& file) {
    const std::string extension = core::toLowerAscii(core::pathToUtf8Lossy(file.extension()));
    if (extension == ".xml") {
        return TextKind::Xml;
    }
    if (extension == ".json") {
        return TextKind::Json;
    }
    if (extension == ".txt" || extension == ".cfg") {
        return TextKind::Plain;
    }
    return TextKind::None;
}

std::string escapedFor(TextKind kind, std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (kind == TextKind::Xml) {
            switch (c) {
            case '&':
                out += "&amp;";
                continue;
            case '<':
                out += "&lt;";
                continue;
            case '>':
                out += "&gt;";
                continue;
            case '"':
                out += "&quot;";
                continue;
            case '\'':
                out += "&apos;";
                continue;
            default:
                break;
            }
        } else if (kind == TextKind::Json) {
            if (c == '\\' || c == '"') {
                out += '\\';
            } else if (static_cast<unsigned char>(c) < 0x20) {
                out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
                continue;
            }
        }
        out += c;
    }
    return out;
}

/// The key of a relative path on a case-insensitive file system: two paths with one key name one file.
std::string caseKey(std::string_view relativeUtf8) {
    auto upper = core::fileSystemUpperCase(relativeUtf8);
    return upper ? std::move(*upper) : std::string(relativeUtf8);
}

/// Copies `source` to `target`, the root token of a text file replaced. Written through \\?\ paths, as everything the
/// materializer writes: fixtures may reach past MAX_PATH whatever the system's long path setting.
Status copyFile(const std::filesystem::path& source, const std::filesystem::path& target, const std::string& root) {
    const TextKind kind = textKindOf(source);
    if (kind == TextKind::None) {
        const auto bytes = core::readBinaryFile(source);
        if (!bytes) {
            return propagate(bytes.error());
        }
        return writeFileAnyLength(target,
                                  std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    }
    auto text = core::readTextFile(source);
    if (!text) {
        return propagate(std::move(text.error()));
    }
    const std::string replacement = escapedFor(kind, root);
    std::string& content = *text;
    for (std::size_t at = content.find(kRootToken); at != std::string::npos;
         at = content.find(kRootToken, at + replacement.size())) {
        content.replace(at, kRootToken.size(), replacement);
    }
    return writeFileAnyLength(target, content);
}

Status setLastWriteTime(const std::filesystem::path& target, std::string_view utc) {
    const auto time = core::parseUtcIso8601(utc);
    if (!time) {
        return propagate(time.error());
    }
    return setLastWriteTimeAnyLength(target, std::chrono::clock_cast<std::chrono::file_clock>(*time));
}

/// Generates what files.json lists into `root`; `copied` holds the case keys of the paths copied from the source tree.
Status generate(const std::filesystem::path& manifest, const std::filesystem::path& root,
                const std::set<std::string>& copied) {
    const auto document = core::readJsonFile(manifest);
    if (!document) {
        return propagate(document.error());
    }
    const std::string context = core::pathToUtf8Lossy(manifest);
    const auto fail = [&](std::string message) { return makeError(ErrorCode::ValidationFailed, message, context); };
    if (!document->is_object()) {
        return fail("files.json must hold an object");
    }
    for (const auto& [key, value] : document->items()) {
        if (key != "files" && key != "directories") {
            return fail("unknown key \"" + key + "\" (files, directories)");
        }
        if (!value.is_array()) {
            return fail("\"" + key + "\" must be an array");
        }
    }
    const auto relativeOf = [&](const nlohmann::json& value) -> Expected<std::string> {
        if (!value.is_string()) {
            return fail("a path must be a string");
        }
        return core::resolveRelativePath(value.get<std::string>(), root);
    };

    for (const nlohmann::json& folder : document->value("directories", nlohmann::json::array())) {
        const auto relative = relativeOf(folder);
        if (!relative) {
            return propagate(relative.error());
        }
        if (auto created = createFoldersAnyLength(root / core::pathFromUtf8(*relative)); !created) {
            return created;
        }
    }
    std::set<std::string> generated; // case keys, as `copied`
    for (const nlohmann::json& file : document->value("files", nlohmann::json::array())) {
        if (!file.is_object()) {
            return fail("a file must be an object");
        }
        for (const auto& [key, value] : file.items()) {
            static constexpr std::array<std::string_view, 5> kKeys{"path", "size", "seed", "mtime", "hidden"};
            if (std::find(kKeys.begin(), kKeys.end(), key) == kKeys.end()) {
                return fail("unknown key \"" + key + "\" of a file (path, size, seed, mtime, hidden)");
            }
        }
        const auto relative = relativeOf(file.value("path", nlohmann::json()));
        if (!relative) {
            return propagate(relative.error());
        }
        // On a case-insensitive file system "ROMS/A.BIN" is "roms/a.bin": compared by their keys.
        const std::string key = caseKey(*relative);
        if (copied.contains(key)) {
            return fail("the generated file " + *relative + " would replace a file of the tree");
        }
        if (!generated.insert(key).second) {
            return fail("the generated file " + *relative + " is listed twice (names differing in case only)");
        }
        const nlohmann::json size = file.value("size", nlohmann::json());
        const nlohmann::json seed = file.value("seed", nlohmann::json());
        if (!size.is_number_unsigned() || !seed.is_number_unsigned()) {
            return fail(*relative + ": \"size\" and \"seed\" must be integers of 0 or more");
        }
        if (size.get<std::uint64_t>() > kMaxPayloadBytes) {
            return fail(*relative + ": larger than 64 MiB");
        }
        const std::filesystem::path target = root / core::pathFromUtf8(*relative);
        const std::string payload =
            payloadBytes(seed.get<std::uint64_t>(), static_cast<std::size_t>(size.get<std::uint64_t>()));
        if (auto written = writeFileAnyLength(target, payload); !written) {
            return written;
        }
        if (file.contains("mtime")) {
            if (!file["mtime"].is_string()) {
                return fail(*relative + ": \"mtime\" must be a UTC time such as \"2001-02-03T04:05:06Z\"");
            }
            if (auto set = setLastWriteTime(target, file["mtime"].get<std::string>()); !set) {
                return set;
            }
        }
        if (file.contains("hidden")) {
            if (!file["hidden"].is_boolean()) {
                return fail(*relative + ": \"hidden\" must be true or false");
            }
            if (file["hidden"].get<bool>()) {
                if (auto hidden = setHidden(target, true); !hidden) {
                    return hidden;
                }
            }
        }
    }
    return ok();
}

} // namespace

std::filesystem::path fixturesDir() {
    return core::pathFromUtf8(RV_TEST_SUPPORT_SOURCE_DIR) / "tests" / "fixtures";
}

std::string payloadBytes(std::uint64_t seed, std::size_t size) {
    std::string bytes(size, '\0');
    std::uint64_t state = seed;
    for (std::size_t i = 0; i < size; i += 8) {
        // SplitMix64 (Steele, Lea and Flood).
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        for (std::size_t k = 0; k < 8 && i + k < size; ++k) {
            bytes[i + k] = static_cast<char>((z >> (8 * k)) & 0xFF);
        }
    }
    return bytes;
}

Expected<Fixture> Fixture::materialize(std::string_view name) {
    return materializeFrom(fixturesDir() / core::pathFromUtf8(name), name);
}

Expected<Fixture> Fixture::materializeFrom(const std::filesystem::path& source, std::string_view name) {
    static std::atomic<unsigned> s_counter{0};
    std::error_code ec;
    if (!std::filesystem::is_directory(source, ec)) {
        return makeError(ErrorCode::NotFound, "no fixture there", core::pathToUtf8Lossy(source));
    }
    const std::filesystem::path root =
        buildDir() / "test-fixtures" /
        core::pathFromUtf8(fmt::format("{}-{}-{}", name, GetCurrentProcessId(), s_counter.fetch_add(1)));
    removeTreeAnyLength(root); // left by an earlier process that had the same id
    if (auto created = createFoldersAnyLength(root); !created) {
        return propagate(created.error());
    }
    Fixture fixture(root); // from here on its destructor removes the folder
    const std::string rootText = core::pathToUtf8Lossy(root);

    std::set<std::string> copied; // case keys of the copied files
    for (auto entry = std::filesystem::recursive_directory_iterator(source, ec);
         !ec && entry != std::filesystem::recursive_directory_iterator(); entry.increment(ec)) {
        const std::filesystem::path relative = entry->path().lexically_relative(source);
        if (relative == std::filesystem::path(kManifestName)) {
            continue;
        }
        const std::filesystem::path target = root / relative;
        if (entry->is_directory(ec)) {
            if (auto created = createFoldersAnyLength(target); !created) {
                return propagate(created.error());
            }
            continue;
        }
        if (auto done = copyFile(entry->path(), target, rootText); !done) {
            return propagate(done.error());
        }
        copied.insert(caseKey(core::pathToGenericUtf8(relative)));
    }
    if (ec) {
        return makeError(ErrorCode::IoError, "cannot copy the fixture: " + ec.message(), core::pathToUtf8Lossy(source));
    }
    if (const std::filesystem::path manifest = source / kManifestName; std::filesystem::exists(manifest, ec)) {
        if (auto generated = generate(manifest, root, copied); !generated) {
            return propagate(generated.error());
        }
    }
    return fixture;
}

Fixture::Fixture(Fixture&& other) noexcept : m_root(std::move(other.m_root)), m_keep(other.m_keep) {
    other.m_root.clear();
}

Fixture::~Fixture() {
    if (!m_root.empty() && !m_keep) {
        removeTreeAnyLength(m_root);
    }
}

std::filesystem::path Fixture::path(std::string_view relative) const {
    return m_root / core::pathFromUtf8(relative);
}

} // namespace rv::test
