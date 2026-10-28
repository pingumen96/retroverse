#include <rv/core/canonical_json.hpp>
#include <rv/core/case_fold.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/utf8.hpp>
#include <rv/media/video_verdicts.hpp>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace rv::media {
namespace {

using Json = nlohmann::json;

/// The failures a verdict may name: every one but None (playable) and FileNotFound (a missing file has no verdict).
constexpr std::array<DecodeFailure, 6> kVerdictFailures{DecodeFailure::InvalidFile,      DecodeFailure::NoVideoStream,
                                                        DecodeFailure::UnsupportedCodec, DecodeFailure::CorruptData,
                                                        DecodeFailure::Truncated,        DecodeFailure::DecoderError};

/// The members of a verdict, in no particular order (the canonical writer sorts them).
constexpr std::array<std::string_view, 12> kVerdictMembers{
    "audioSampleRate", "bytes",    "codec", "damagedFrames", "decodeErrors", "durationMs",
    "height",          "modified", "path",  "source",        "verdict",      "width"};

Status malformedPath(std::string_view path, std::string_view why) {
    return makeError(ErrorCode::InvalidArgument, fmt::format("a verdict's path {}", why), std::string(path));
}

/// The path is UTF-8, '/' between non-empty elements, none of them "." or "..", no '\'.
Status checkPath(std::string_view path) {
    if (path.empty()) {
        return malformedPath(path, "is empty");
    }
    if (!core::isValidUtf8(path)) {
        return malformedPath(path, "is not UTF-8");
    }
    if (path.find('\\') != std::string_view::npos || path.find('\0') != std::string_view::npos) {
        return malformedPath(path, "holds '\\' or NUL");
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = path.find('/', start);
        const std::string_view element =
            path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (element.empty() || element == "." || element == "..") {
            return malformedPath(path, "has an empty, \".\" or \"..\" element");
        }
        if (slash == std::string_view::npos) {
            return {};
        }
        start = slash + 1;
    }
}

Error invalid(std::string_view context, std::string message) {
    return Error{ErrorCode::ValidationFailed, std::move(message), std::string(context)};
}

Expected<std::int64_t> integerMember(const Json& entry, std::string_view key, std::int64_t minimum,
                                     std::string_view context, std::size_t index) {
    const auto found = entry.find(std::string(key));
    if (found == entry.end() || !found->is_number_integer()) {
        return tl::unexpected(invalid(context, fmt::format("videos[{}]: \"{}\" must be an integer", index, key)));
    }
    if (found->is_number_unsigned() &&
        found->get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return tl::unexpected(invalid(context, fmt::format("videos[{}]: \"{}\" is too large", index, key)));
    }
    const std::int64_t value = found->get<std::int64_t>();
    if (value < minimum) {
        return tl::unexpected(
            invalid(context, fmt::format("videos[{}]: \"{}\" must be at least {}", index, key, minimum)));
    }
    return value;
}

Expected<std::string> stringMember(const Json& entry, std::string_view key, std::string_view context,
                                   std::size_t index) {
    const auto found = entry.find(std::string(key));
    if (found == entry.end() || !found->is_string()) {
        return tl::unexpected(invalid(context, fmt::format("videos[{}]: \"{}\" must be a string", index, key)));
    }
    return found->get<std::string>();
}

Expected<VideoVerdict> verdictFrom(const Json& entry, std::string_view context, std::size_t index) {
    if (!entry.is_object()) {
        return tl::unexpected(invalid(context, fmt::format("videos[{}] must be an object", index)));
    }
    for (const auto& [key, value] : entry.items()) {
        if (std::find(kVerdictMembers.begin(), kVerdictMembers.end(), key) == kVerdictMembers.end()) {
            return tl::unexpected(invalid(context, fmt::format("videos[{}]: unknown member \"{}\"", index, key)));
        }
    }
    VideoVerdict verdict;
    auto path = stringMember(entry, "path", context, index);
    auto codec = stringMember(entry, "codec", context, index);
    auto verdictText = stringMember(entry, "verdict", context, index);
    auto source = stringMember(entry, "source", context, index);
    for (const auto* text : {&path, &codec, &verdictText, &source}) {
        if (!*text) {
            return tl::unexpected(text->error());
        }
    }
    verdict.path = std::move(*path);
    if (const Status checked = checkPath(verdict.path); !checked) {
        return tl::unexpected(invalid(context, fmt::format("videos[{}]: {}", index, checked.error().message)));
    }
    verdict.codec = std::move(*codec);
    const std::optional<DecodeFailure> failure = parseVerdictName(*verdictText);
    if (!failure) {
        return tl::unexpected(invalid(context, fmt::format("videos[{}]: unknown verdict \"{}\"", index, *verdictText)));
    }
    verdict.failure = *failure;
    if (*source == "check") {
        verdict.source = VerdictSource::Check;
    } else if (*source == "play") {
        verdict.source = VerdictSource::Play;
    } else {
        return tl::unexpected(invalid(context, fmt::format("videos[{}]: unknown source \"{}\"", index, *source)));
    }
    struct Field {
        std::string_view key;
        std::int64_t minimum;
        std::int64_t* target;
    };
    std::int64_t bytes = 0;
    std::int64_t errors = 0;
    std::int64_t damaged = 0;
    const std::array<Field, 8> fields{
        {{"bytes", 0, &bytes},
         {"modified", std::numeric_limits<std::int64_t>::min(), &verdict.file.lastWriteTime},
         {"durationMs", 0, &verdict.durationMs},
         {"width", 0, &verdict.width},
         {"height", 0, &verdict.height},
         {"audioSampleRate", 0, &verdict.audioSampleRate},
         {"decodeErrors", 0, &errors},
         {"damagedFrames", 0, &damaged}}};
    for (const Field& field : fields) {
        auto value = integerMember(entry, field.key, field.minimum, context, index);
        if (!value) {
            return tl::unexpected(value.error());
        }
        *field.target = *value;
    }
    verdict.file.size = static_cast<std::uint64_t>(bytes);
    verdict.decodeErrors = static_cast<std::uint64_t>(errors);
    verdict.damagedFrames = static_cast<std::uint64_t>(damaged);
    return verdict;
}

Json verdictJson(const VideoVerdict& verdict) {
    return Json{{"path", verdict.path},
                {"bytes", static_cast<std::int64_t>(verdict.file.size)},
                {"modified", verdict.file.lastWriteTime},
                {"verdict", verdictName(verdict.failure)},
                {"durationMs", verdict.durationMs},
                {"width", verdict.width},
                {"height", verdict.height},
                {"audioSampleRate", verdict.audioSampleRate},
                {"codec", verdict.codec},
                {"decodeErrors", static_cast<std::int64_t>(verdict.decodeErrors)},
                {"damagedFrames", static_cast<std::int64_t>(verdict.damagedFrames)},
                {"source", std::string(toString(verdict.source))}};
}

/// A named mutex of the verdicts file, shared by the processes that write it (the app and the CLI): held while one of
/// them reads, merges and writes.
class FileLock {
public:
    FileLock(const std::filesystem::path& file, std::chrono::milliseconds wait) {
        const std::string absolute = core::pathToUtf8Lossy(std::filesystem::absolute(file).lexically_normal());
        const std::string folded = core::fileSystemUpperCase(absolute).value_or(absolute);
        const std::string name = "Local\\RetroverseVideoVerdicts-" + core::sha256Hex(folded).substr(0, 32);
        const auto wide = core::utf8ToWide(name);
        m_handle = wide ? CreateMutexW(nullptr, FALSE, wide->c_str()) : nullptr;
        if (m_handle == nullptr) {
            return; // no lock to be had: the write goes on alone (logged by the caller)
        }
        const DWORD waited = WaitForSingleObject(m_handle, static_cast<DWORD>(wait.count()));
        m_held = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
    }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    ~FileLock() {
        if (m_held) {
            ReleaseMutex(m_handle);
        }
        if (m_handle != nullptr) {
            CloseHandle(m_handle);
        }
    }

    [[nodiscard]] bool created() const noexcept { return m_handle != nullptr; }
    [[nodiscard]] bool held() const noexcept { return m_held; }

private:
    HANDLE m_handle = nullptr;
    bool m_held = false;
};

} // namespace

bool isUnreadableText(ErrorCode code) noexcept {
    return code == ErrorCode::ParseError || code == ErrorCode::ValidationFailed;
}

std::string_view toString(VerdictSource source) noexcept {
    return source == VerdictSource::Play ? "play" : "check";
}

std::string verdictName(DecodeFailure failure) {
    if (failure == DecodeFailure::None) {
        return "playable";
    }
    std::string name(toString(failure));
    std::replace(name.begin(), name.end(), ' ', '-');
    return name;
}

std::optional<DecodeFailure> parseVerdictName(std::string_view name) {
    if (name == "playable") {
        return DecodeFailure::None;
    }
    for (const DecodeFailure failure : kVerdictFailures) {
        if (verdictName(failure) == name) {
            return failure;
        }
    }
    return std::nullopt;
}

void describeStream(VideoVerdict& verdict, const StreamInfo& info) {
    verdict.durationMs = info.durationSeconds > 0.0 && std::isfinite(info.durationSeconds)
                             ? static_cast<std::int64_t>(std::llround(info.durationSeconds * 1000.0))
                             : 0;
    verdict.width = std::max(0, info.codedSize.width);
    verdict.height = std::max(0, info.codedSize.height);
    verdict.audioSampleRate = std::max(0, info.audioSampleRate);
    verdict.codec = info.codec;
}

std::optional<std::string> verdictKey(std::string_view path) {
    if (!checkPath(path)) {
        return std::nullopt;
    }
    auto folded = core::fileSystemUpperCase(path);
    return folded ? std::optional<std::string>(std::move(*folded)) : std::nullopt;
}

Expected<VideoVerdicts> VideoVerdicts::parse(std::string_view text, std::string_view context) {
    auto document = core::parseCanonicalJson(text, context);
    if (!document) {
        return propagate(document.error());
    }
    const Json& json = *document;
    if (!json.is_object() || json.size() != 3 || !json.contains("format") || !json.contains("version") ||
        !json.contains("videos")) {
        return tl::unexpected(invalid(context, "expected an object of \"format\", \"version\" and \"videos\""));
    }
    if (!json.at("format").is_string() || json.at("format").get<std::string>() != kVideoVerdictsFormat) {
        return tl::unexpected(invalid(context, fmt::format("\"format\" must be \"{}\"", kVideoVerdictsFormat)));
    }
    if (!json.at("version").is_number_integer() || json.at("version").get<std::int64_t>() != kVideoVerdictsVersion) {
        return tl::unexpected(invalid(context, fmt::format("\"version\" must be {}", kVideoVerdictsVersion)));
    }
    const Json& videos = json.at("videos");
    if (!videos.is_array()) {
        return tl::unexpected(invalid(context, "\"videos\" must be an array"));
    }
    VideoVerdicts verdicts;
    for (std::size_t index = 0; index < videos.size(); ++index) {
        auto verdict = verdictFrom(videos[index], context, index);
        if (!verdict) {
            return propagate(verdict.error());
        }
        auto key = verdictKey(verdict->path);
        if (!key) {
            return tl::unexpected(invalid(context, fmt::format("videos[{}]: the path cannot be folded", index)));
        }
        if (!verdicts.m_byKey.emplace(std::move(*key), std::move(*verdict)).second) {
            return tl::unexpected(invalid(context, fmt::format("videos[{}]: a second verdict of its path", index)));
        }
    }
    return verdicts;
}

Expected<std::string> VideoVerdicts::format() const {
    Json videos = Json::array();
    for (const auto& [key, verdict] : m_byKey) {
        videos.push_back(verdictJson(verdict));
    }
    const Json document{{"format", std::string(kVideoVerdictsFormat)},
                        {"version", kVideoVerdictsVersion},
                        {"videos", std::move(videos)}};
    return core::writeCanonicalJson(document);
}

const VideoVerdict* VideoVerdicts::find(std::string_view path) const {
    const std::optional<std::string> key = verdictKey(path);
    if (!key) {
        return nullptr;
    }
    const auto found = m_byKey.find(*key);
    return found != m_byKey.end() ? &found->second : nullptr;
}

const VideoVerdict* VideoVerdicts::findCurrent(std::string_view path, const VideoFileFacts& facts) const {
    const VideoVerdict* verdict = find(path);
    return verdict != nullptr && verdict->file == facts ? verdict : nullptr;
}

Status VideoVerdicts::set(VideoVerdict verdict) {
    if (const Status checked = checkPath(verdict.path); !checked) {
        return checked;
    }
    if (verdict.failure == DecodeFailure::FileNotFound || verdict.failure == DecodeFailure::ReadError) {
        return makeError(ErrorCode::InvalidArgument, "a missing or unreadable file has no verdict", verdict.path);
    }
    auto key = verdictKey(verdict.path);
    if (!key) {
        return malformedPath(verdict.path, "cannot be folded");
    }
    m_byKey.insert_or_assign(std::move(*key), std::move(verdict));
    return {};
}

bool VideoVerdicts::erase(std::string_view path) {
    const std::optional<std::string> key = verdictKey(path);
    return key && m_byKey.erase(*key) > 0;
}

std::vector<const VideoVerdict*> VideoVerdicts::entries() const {
    std::vector<const VideoVerdict*> list;
    list.reserve(m_byKey.size());
    for (const auto& [key, verdict] : m_byKey) {
        list.push_back(&verdict);
    }
    return list;
}

std::filesystem::path videoVerdictsFile(const std::filesystem::path& cacheDir) {
    return cacheDir / "media" / "videos.json";
}

namespace {

/// The file read, the lock held by the caller.
Expected<VideoVerdicts> readHeld(const std::filesystem::path& file) {
    auto text = core::readFileShared(file, core::kDefaultTextLimitBytes);
    if (!text) {
        if (text.error().code == ErrorCode::NotFound) {
            return VideoVerdicts{};
        }
        return propagate(text.error());
    }
    return VideoVerdicts::parse(*text, core::pathToUtf8Lossy(file));
}

/// The lock of `file` had, or why not.
Status lockHeld(const FileLock& lock, const std::filesystem::path& file) {
    if (lock.created() && !lock.held()) {
        return makeError(ErrorCode::Conflict, "another process writes the clips' verdicts",
                         core::pathToUtf8Lossy(file));
    }
    if (!lock.created()) {
        spdlog::warn("Video verdicts: no lock for {}; used without one", core::pathToUtf8Lossy(file));
    }
    return ok();
}

} // namespace

Expected<VideoVerdicts> readVideoVerdicts(const std::filesystem::path& file, std::chrono::milliseconds lockWait) {
    // Under the writers' lock: a writer's replace (a rename over the file) fails while another program holds it open,
    // even for a read that shares everything.
    const FileLock lock(file, lockWait);
    if (const Status held = lockHeld(lock, file); !held) {
        return propagate(held.error());
    }
    return readHeld(file);
}

Expected<VideoVerdicts> updateVideoVerdicts(const std::filesystem::path& file, std::span<const VideoVerdict> changes,
                                            std::span<const std::string> removals, std::chrono::milliseconds lockWait) {
    const FileLock lock(file, lockWait);
    if (const Status held = lockHeld(lock, file); !held) {
        return propagate(held.error());
    }
    auto verdicts = readHeld(file);
    if (!verdicts) {
        // Only a text that is not the verdicts' is replaced; a file that could not be read (held, a device error, too
        // large) keeps its verdicts, and this write waits for the next one.
        if (!isUnreadableText(verdicts.error().code)) {
            return propagate(verdicts.error());
        }
        spdlog::warn("Video verdicts: {} is replaced ({})", core::pathToUtf8Lossy(file), verdicts.error().toString());
        verdicts = VideoVerdicts{};
    }
    for (const std::string& path : removals) {
        verdicts->erase(path);
    }
    for (const VideoVerdict& change : changes) {
        if (const Status set = verdicts->set(change); !set) {
            return propagate(set.error());
        }
    }
    const auto text = verdicts->format();
    if (!text) {
        return propagate(text.error());
    }
    if (const Status written = core::writeTextFileAtomic(file, *text); !written) {
        return propagate(written.error());
    }
    return std::move(*verdicts);
}

} // namespace rv::media
