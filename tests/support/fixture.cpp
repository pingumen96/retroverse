#include "support/fixture.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <set>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

#include <windows.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/core/case_fold.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/relative_path.hpp>
#include <rv/core/system_error.hpp>
#include <rv/core/text.hpp>
#include <rv/core/time.hpp>
#include <rv/core/utf8.hpp>

#include "support/file_system.hpp"
#include "support/fixture_collection.hpp"
#include "support/fixture_payloads.hpp"

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

/// Copies the program files.json's "copyOf" names (`name`, a file name of testProgramsDir()) to `target`, the file
/// `relative` of the fixture; errors name `context`, the manifest.
Status copyProgram(const nlohmann::json& name, const std::filesystem::path& target, const std::string& relative,
                   const std::string& context) {
    const auto fail = [&](std::string message) {
        return makeError(ErrorCode::ValidationFailed, relative + ": " + message, context);
    };
    if (!name.is_string()) {
        return fail("\"copyOf\" must be a file name");
    }
    const std::string text = name.get<std::string>();
    if (text.empty() || text == "." || text == ".." || text.find_first_of("/\\:") != std::string::npos) {
        return fail("\"copyOf\" must be a file name of the folder of the test programs, not a path");
    }
    const std::filesystem::path source = testProgramsDir() / core::pathFromUtf8(text);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec)) {
        return makeError(ErrorCode::NotFound, relative + ": " + text + " is not beside the test program (built?)",
                         core::pathToUtf8Lossy(source));
    }
    const auto bytes = core::readBinaryFile(source);
    if (!bytes) {
        return propagate(bytes.error());
    }
    return writeFileAnyLength(target, std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
}

/// One file files.json generates: its bytes from a seed, a program of the build, a picture, a clip or a text (a
/// collection's gamelist); then optionally its time and the hidden attribute.
struct FileJob {
    struct Payload {
        std::uint64_t seed = 0;
        std::size_t size = 0;
    };
    struct Program {
        nlohmann::json name;
    };
    struct Text {
        std::string content;
    };

    std::string relative;
    std::filesystem::path target;
    std::variant<Payload, Program, PicturePayload, ClipPayload, Text> content;
    /// More files of the same bytes (a picture both releases of a collection's title share): generated once.
    std::vector<std::filesystem::path> copies;
    std::optional<std::string> mtime;
    bool hidden = false;
};

Status runJob(const FileJob& job, const std::string& context) {
    if (const auto* program = std::get_if<FileJob::Program>(&job.content)) {
        return copyProgram(program->name, job.target, job.relative, context);
    }
    if (const auto* clip = std::get_if<ClipPayload>(&job.content)) {
        return writeClip(job.target, *clip);
    }
    std::string bytes;
    if (const auto* payload = std::get_if<FileJob::Payload>(&job.content)) {
        bytes = payloadBytes(payload->seed, payload->size);
    } else if (const auto* picture = std::get_if<PicturePayload>(&job.content)) {
        auto encoded = pictureBytes(*picture);
        if (!encoded) {
            return makeError(encoded.error().code, job.relative + ": " + encoded.error().message, context);
        }
        bytes = std::move(*encoded);
    } else {
        bytes = std::get<FileJob::Text>(job.content).content;
    }
    if (auto written = writeFileAnyLength(job.target, bytes); !written) {
        return written;
    }
    for (const std::filesystem::path& copy : job.copies) {
        if (auto written = writeFileAnyLength(copy, bytes); !written) {
            return written;
        }
    }
    return ok();
}

/// Runs the jobs on a few threads (pictures and clips take a while, the 2,300 covers of snes-stress most of all), then
/// sets the times and attributes in order. The first failure in the jobs' order is returned, whatever the threads'.
Status runJobs(const std::vector<FileJob>& jobs, const std::string& context) {
    // The folders first, from one thread.
    std::set<std::filesystem::path> folders;
    for (const FileJob& job : jobs) {
        folders.insert(job.target.parent_path());
        for (const std::filesystem::path& copy : job.copies) {
            folders.insert(copy.parent_path());
        }
    }
    for (const std::filesystem::path& folder : folders) {
        if (auto created = createFoldersAnyLength(folder); !created) {
            return created;
        }
    }
    std::vector<Status> results(jobs.size(), ok());
    std::atomic<std::size_t> next{0};
    const auto work = [&] {
        for (std::size_t index = next.fetch_add(1); index < jobs.size(); index = next.fetch_add(1)) {
            results[index] = runJob(jobs[index], context);
        }
    };
    const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    {
        std::vector<std::jthread> pool;
        for (unsigned i = 1; i < threads && i < jobs.size(); ++i) {
            pool.emplace_back(work);
        }
        work();
    }
    for (std::size_t index = 0; index < jobs.size(); ++index) {
        if (!results[index]) {
            return results[index];
        }
        const FileJob& job = jobs[index];
        if (job.mtime) {
            if (auto set = setLastWriteTime(job.target, *job.mtime); !set) {
                return set;
            }
        }
        if (job.hidden) {
            if (auto hidden = setHidden(job.target, true); !hidden) {
                return hidden;
            }
        }
    }
    return ok();
}

/// Generates what files.json lists into `root`; `copied` holds the case keys of the paths copied from the source tree.
Status generate(const std::filesystem::path& manifest, const std::filesystem::path& root,
                const std::set<std::string>& copied) {
    const auto document = core::readJsonFile(manifest);
    if (!document) {
        return propagate(document.error());
    }
    const std::string context = core::pathToUtf8Lossy(manifest);
    const auto fail = [&](std::string message) {
        return makeError(ErrorCode::ValidationFailed, message, context);
    };
    if (!document->is_object()) {
        return fail("files.json must hold an object");
    }
    for (const auto& [key, value] : document->items()) {
        if (key != "files" && key != "directories" && key != "collections") {
            return fail("unknown key \"" + key + "\" (files, directories, collections)");
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

    // Every file is checked and described first; nothing is written until the whole manifest is known to be valid.
    std::vector<FileJob> jobs;
    std::set<std::string> generated; // case keys, as `copied`
    // The relative path of a generated file `path` (as files.json spells it), its collisions checked.
    const auto claim = [&](const nlohmann::json& path) -> Expected<std::string> {
        auto relative = relativeOf(path);
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
        return relative;
    };
    // A new job for `path`. The pointer stays valid until the next call.
    const auto addJob = [&](const nlohmann::json& path) -> Expected<FileJob*> {
        auto relative = claim(path);
        if (!relative) {
            return propagate(relative.error());
        }
        FileJob& job = jobs.emplace_back();
        job.target = root / core::pathFromUtf8(*relative);
        job.relative = std::move(*relative);
        return &job;
    };

    for (const nlohmann::json& file : document->value("files", nlohmann::json::array())) {
        if (!file.is_object()) {
            return fail("a file must be an object");
        }
        for (const auto& [key, value] : file.items()) {
            static constexpr std::array<std::string_view, 8> kKeys{"path",  "size",  "seed",  "copyOf",
                                                                   "cover", "video", "mtime", "hidden"};
            if (std::find(kKeys.begin(), kKeys.end(), key) == kKeys.end()) {
                return fail("unknown key \"" + key +
                            "\" of a file (path, size, seed, copyOf, cover, video, mtime, hidden)");
            }
        }
        auto added = addJob(file.value("path", nlohmann::json()));
        if (!added) {
            return propagate(added.error());
        }
        FileJob& job = **added;
        const std::string extension = core::pathToUtf8Lossy(job.target.extension());
        const int kinds = (file.contains("size") || file.contains("seed") ? 1 : 0) + (file.contains("copyOf") ? 1 : 0) +
                          (file.contains("cover") ? 1 : 0) + (file.contains("video") ? 1 : 0);
        if (kinds != 1) {
            return fail(job.relative + ": either \"copyOf\", \"size\" and \"seed\", \"cover\" or \"video\"");
        }
        if (file.contains("copyOf")) {
            job.content = FileJob::Program{file["copyOf"]};
        } else if (file.contains("cover")) {
            auto picture = parsePicturePayload(file["cover"], extension, job.relative);
            if (!picture) {
                return makeError(picture.error().code, picture.error().message, context);
            }
            job.content = std::move(*picture);
        } else if (file.contains("video")) {
            auto clip = parseClipPayload(file["video"], extension, job.relative);
            if (!clip) {
                return makeError(clip.error().code, clip.error().message, context);
            }
            job.content = std::move(*clip);
        } else {
            const nlohmann::json size = file.value("size", nlohmann::json());
            const nlohmann::json seed = file.value("seed", nlohmann::json());
            if (!size.is_number_unsigned() || !seed.is_number_unsigned()) {
                return fail(job.relative + ": \"size\" and \"seed\" must be integers of 0 or more");
            }
            if (size.get<std::uint64_t>() > kMaxPayloadBytes) {
                return fail(job.relative + ": larger than 64 MiB");
            }
            job.content =
                FileJob::Payload{seed.get<std::uint64_t>(), static_cast<std::size_t>(size.get<std::uint64_t>())};
        }
        if (file.contains("mtime")) {
            if (!file["mtime"].is_string()) {
                return fail(job.relative + ": \"mtime\" must be a UTC time such as \"2001-02-03T04:05:06Z\"");
            }
            job.mtime = file["mtime"].get<std::string>();
        }
        if (file.contains("hidden")) {
            if (!file["hidden"].is_boolean()) {
                return fail(job.relative + ": \"hidden\" must be true or false");
            }
            job.hidden = file["hidden"].get<bool>();
        }
    }

    for (const nlohmann::json& item : document->value("collections", nlohmann::json::array())) {
        auto spec = parseCollectionSpec(item, "a collection");
        if (!spec) {
            return makeError(spec.error().code, spec.error().message, context);
        }
        GeneratedCollection collection = generateCollection(*spec);
        const auto add = [&](const std::string& path, auto content) -> Status {
            auto added = addJob(nlohmann::json(path));
            if (!added) {
                return propagate(added.error());
            }
            (*added)->content = std::move(content);
            return ok();
        };
        if (auto done = add(spec->gamelist, FileJob::Text{std::move(collection.gamelist)}); !done) {
            return done;
        }
        const CollectionGame* previous = nullptr;
        std::size_t previousCover = 0; // the job of `previous`'s cover
        for (const CollectionGame& game : collection.games) {
            Status done = add(spec->romFolder + "/" + game.stem + spec->romExtension,
                              FileJob::Payload{game.romSeed, static_cast<std::size_t>(spec->romSize)});
            const std::string cover = spec->mediaFolder + "/covers/" + game.stem + game.coverExtension;
            if (done && previous != nullptr && previous->titleIndex == game.titleIndex) {
                // The second release of a title shows the same picture: encoded once, written twice.
                auto relative = claim(nlohmann::json(cover));
                if (relative) {
                    jobs[previousCover].copies.push_back(root / core::pathFromUtf8(*relative));
                } else {
                    done = propagate(relative.error());
                }
            } else if (done) {
                done = add(cover, game.cover);
                previousCover = jobs.size() - 1;
            }
            previous = &game;
            if (done && game.hasVideo) {
                done = add(spec->mediaFolder + "/videos/" + game.stem + ".mp4", game.video);
            }
            if (!done) {
                return done;
            }
        }
    }
    return runJobs(jobs, context);
}

} // namespace

std::filesystem::path fixturesDir() {
    return core::pathFromUtf8(RV_TEST_SUPPORT_SOURCE_DIR) / "tests" / "fixtures";
}

std::filesystem::path testProgramsDir() {
    return core::executablePath().parent_path();
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

Status Fixture::applyOverlay(std::string_view relative) const {
    const auto resolved = core::resolveRelativePath(relative, m_root);
    if (!resolved) {
        return propagate(resolved.error());
    }
    const std::filesystem::path overlay = path(*resolved);
    std::error_code ec;
    if (resolved->empty() || !std::filesystem::is_directory(overlay, ec)) {
        return makeError(ErrorCode::NotFound, "no overlay folder there", core::pathToUtf8Lossy(overlay));
    }
    std::vector<std::filesystem::path> files;
    for (auto entry = std::filesystem::recursive_directory_iterator(overlay, ec);
         !ec && entry != std::filesystem::recursive_directory_iterator(); entry.increment(ec)) {
        if (entry->is_regular_file(ec)) {
            files.push_back(entry->path());
        }
    }
    if (ec) {
        return makeError(ErrorCode::IoError, "cannot list the overlay: " + ec.message(),
                         core::pathToUtf8Lossy(overlay));
    }
    for (const std::filesystem::path& file : files) {
        const std::filesystem::path target = m_root / file.lexically_relative(overlay);
        if (auto created = createFoldersAnyLength(target.parent_path()); !created) {
            return created;
        }
        if (!MoveFileExW(core::extendedLengthPath(file).c_str(), core::extendedLengthPath(target).c_str(),
                         MOVEFILE_REPLACE_EXISTING)) {
            return makeError(ErrorCode::IoError, "cannot move the overlay's file: " + core::lastSystemErrorMessage(),
                             core::pathToUtf8Lossy(file));
        }
    }
    removeTreeAnyLength(overlay);
    return ok();
}

Status Fixture::replaceText(std::string_view relative, std::string_view text, std::string_view replacement) const {
    const std::filesystem::path file = path(relative);
    auto content = core::readTextFile(file);
    if (!content) {
        return propagate(std::move(content.error()));
    }
    const std::size_t at = text.empty() ? std::string::npos : content->find(text);
    if (at == std::string::npos) {
        return makeError(ErrorCode::NotFound, "the text to replace is not in the file: " + std::string(text),
                         core::pathToUtf8Lossy(file));
    }
    if (content->find(text, at + 1) != std::string::npos) {
        return makeError(ErrorCode::InvalidArgument, "the text to replace occurs more than once: " + std::string(text),
                         core::pathToUtf8Lossy(file));
    }
    content->replace(at, text.size(), replacement);
    return writeFileAnyLength(file, *content);
}

} // namespace rv::test
