#include <rv/app/run_report.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>

namespace rv::app {
namespace {

using Json = nlohmann::ordered_json;

constexpr std::uint64_t kMaxCount = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kMaxSize = std::numeric_limits<std::size_t>::max();

Json frameStatsToJson(const gl::FrameStats& stats) {
    Json json;
    json["items"] = stats.items;
    json["culledItems"] = stats.culledItems;
    json["batches"] = stats.batches;
    json["drawCalls"] = stats.drawCalls;
    json["instances"] = stats.instances;
    json["triangles"] = stats.triangles;
    json["shaderSwitches"] = stats.shaderSwitches;
    json["materialSwitches"] = stats.materialSwitches;
    json["debugLines"] = stats.debugLines;
    json["shadowDrawCalls"] = stats.shadowDrawCalls;
    json["shadowInstances"] = stats.shadowInstances;
    json["depthDrawCalls"] = stats.depthDrawCalls;
    Json gpu = Json::object();
    const std::pair<const char*, const std::optional<double>*> times[] = {
        {"shadow", &stats.shadowGpuMilliseconds}, {"depth", &stats.depthGpuMilliseconds},
        {"ssao", &stats.ssaoGpuMilliseconds},     {"scene", &stats.sceneGpuMilliseconds},
        {"post", &stats.postGpuMilliseconds},
    };
    for (const auto& [name, value] : times) {
        if (*value && std::isfinite(**value)) {
            gpu[name] = **value;
        }
    }
    json["gpuMilliseconds"] = std::move(gpu);
    return json;
}

/// Reads the fields of one JSON object. The first error is kept (in the Error the reader shares with its children) and
/// later reads return defaults, so a parse reads every field and checks once at the end.
class FieldReader {
public:
    FieldReader(const nlohmann::json& object, std::string pointer, std::string_view context,
                std::optional<Error>& firstError)
        : m_object(object), m_pointer(std::move(pointer)), m_context(context), m_error(firstError) {
        if (!m_object.is_object()) {
            fail("", "expected an object");
        }
    }

    [[nodiscard]] std::string string(std::string_view key) {
        const nlohmann::json* value = required(key);
        if (value == nullptr) {
            return {};
        }
        if (!value->is_string()) {
            fail(key, "expected a string");
            return {};
        }
        return value->get<std::string>();
    }

    [[nodiscard]] std::optional<std::string> optionalString(std::string_view key) {
        return has(key) ? std::optional<std::string>(string(key)) : std::nullopt;
    }

    [[nodiscard]] bool boolean(std::string_view key) {
        const nlohmann::json* value = required(key);
        if (value == nullptr) {
            return false;
        }
        if (!value->is_boolean()) {
            fail(key, "expected true or false");
            return false;
        }
        return value->get<bool>();
    }

    /// A non-negative integer up to `maximum`.
    [[nodiscard]] std::uint64_t count(std::string_view key, std::uint64_t maximum = kMaxCount) {
        const nlohmann::json* value = required(key);
        if (value == nullptr) {
            return 0;
        }
        std::optional<std::uint64_t> number;
        if (value->is_number_unsigned()) {
            number = value->get<std::uint64_t>();
        } else if (value->is_number_integer() && value->get<std::int64_t>() >= 0) {
            number = static_cast<std::uint64_t>(value->get<std::int64_t>());
        }
        if (!number || *number > maximum) {
            fail(key, fmt::format("expected an integer from 0 to {}", maximum));
            return 0;
        }
        return *number;
    }

    [[nodiscard]] std::optional<std::uint64_t> optionalCount(std::string_view key, std::uint64_t maximum = kMaxCount) {
        return has(key) ? std::optional<std::uint64_t>(count(key, maximum)) : std::nullopt;
    }

    [[nodiscard]] int integer(std::string_view key) {
        const nlohmann::json* value = required(key);
        if (value == nullptr) {
            return 0;
        }
        if (!value->is_number_integer() || value->get<std::int64_t>() < std::numeric_limits<int>::min() ||
            value->get<std::int64_t>() > std::numeric_limits<int>::max()) {
            fail(key, "expected an integer");
            return 0;
        }
        return static_cast<int>(value->get<std::int64_t>());
    }

    /// A finite number.
    [[nodiscard]] double number(std::string_view key) {
        const nlohmann::json* value = required(key);
        if (value == nullptr) {
            return 0.0;
        }
        if (!value->is_number() || !std::isfinite(value->get<double>())) {
            fail(key, "expected a finite number");
            return 0.0;
        }
        return value->get<double>();
    }

    [[nodiscard]] std::optional<double> optionalNumber(std::string_view key) {
        return has(key) ? std::optional<double>(number(key)) : std::nullopt;
    }

    /// Three finite numbers.
    [[nodiscard]] glm::vec3 vec3(std::string_view key) {
        const nlohmann::json* value = required(key);
        if (value == nullptr) {
            return glm::vec3(0.0f);
        }
        auto vector = core::jsonToVec3(*value, location(key));
        if (!vector) {
            fail(key, "expected an array of three numbers");
            return glm::vec3(0.0f);
        }
        return *vector;
    }

    /// The reader of the object at `key`; nothing when the key is absent.
    [[nodiscard]] std::optional<FieldReader> optionalObject(std::string_view key) {
        const nlohmann::json* value = find(key);
        if (value == nullptr) {
            return std::nullopt;
        }
        return FieldReader(*value, m_pointer + "/" + std::string(key), m_context, m_error);
    }

    /// The reader of the object at `key`, which must be there.
    [[nodiscard]] std::optional<FieldReader> object(std::string_view key) {
        if (!has(key)) {
            fail(key, "missing required key");
        }
        return optionalObject(key);
    }

    /// The readers of the objects in the array at `key`, which must be there, in order.
    [[nodiscard]] std::vector<FieldReader> objectArray(std::string_view key) {
        if (!has(key)) {
            fail(key, "missing required key");
        }
        return optionalObjectArray(key);
    }

    /// The readers of the objects in the array at `key`, in order; none when the key is absent.
    [[nodiscard]] std::vector<FieldReader> optionalObjectArray(std::string_view key) {
        std::vector<FieldReader> readers;
        const nlohmann::json* value = find(key);
        if (value == nullptr) {
            return readers;
        }
        if (!value->is_array()) {
            fail(key, "expected an array of objects");
            return readers;
        }
        for (std::size_t index = 0; index < value->size(); ++index) {
            readers.emplace_back((*value)[index], fmt::format("{}/{}/{}", m_pointer, key, index), m_context, m_error);
        }
        return readers;
    }

    /// An array of strings.
    [[nodiscard]] std::vector<std::string> stringArray(std::string_view key) {
        std::vector<std::string> strings;
        const nlohmann::json* value = required(key);
        if (value == nullptr) {
            return strings;
        }
        if (!value->is_array()) {
            fail(key, "expected an array of strings");
            return strings;
        }
        for (const nlohmann::json& item : *value) {
            if (!item.is_string()) {
                fail(key, "expected an array of strings");
                return {};
            }
            strings.push_back(item.get<std::string>());
        }
        return strings;
    }

    [[nodiscard]] bool has(std::string_view key) const { return find(key) != nullptr; }

    /// Records a validation error at `key` unless an earlier one is kept.
    void fail(std::string_view key, std::string message) {
        if (!m_error) {
            m_error = Error{ErrorCode::ValidationFailed, std::move(message), location(key)};
        }
    }

private:
    [[nodiscard]] const nlohmann::json* find(std::string_view key) const {
        if (!m_object.is_object()) {
            return nullptr;
        }
        const auto it = m_object.find(key);
        return it == m_object.end() || it->is_null() ? nullptr : &*it;
    }

    [[nodiscard]] const nlohmann::json* required(std::string_view key) {
        const nlohmann::json* value = find(key);
        if (value == nullptr) {
            fail(key, "missing required key");
        }
        return value;
    }

    [[nodiscard]] std::string location(std::string_view key) const {
        std::string pointer = m_pointer;
        if (!key.empty()) {
            pointer += '/';
            pointer += key;
        }
        return fmt::format("{}:{}", m_context, pointer.empty() ? std::string_view("/") : std::string_view(pointer));
    }

    const nlohmann::json& m_object;
    std::string m_pointer;
    std::string_view m_context;
    std::optional<Error>& m_error;
};

gl::FrameStats readFrameStats(FieldReader& fields) {
    gl::FrameStats stats;
    stats.items = fields.count("items", kMaxSize);
    stats.culledItems = fields.count("culledItems", kMaxSize);
    stats.batches = fields.count("batches", kMaxSize);
    stats.drawCalls = fields.count("drawCalls", kMaxSize);
    stats.instances = fields.count("instances", kMaxSize);
    stats.triangles = fields.count("triangles", kMaxSize);
    stats.shaderSwitches = fields.count("shaderSwitches", kMaxSize);
    stats.materialSwitches = fields.count("materialSwitches", kMaxSize);
    stats.debugLines = fields.count("debugLines", kMaxSize);
    stats.shadowDrawCalls = fields.count("shadowDrawCalls", kMaxSize);
    stats.shadowInstances = fields.count("shadowInstances", kMaxSize);
    stats.depthDrawCalls = fields.count("depthDrawCalls", kMaxSize);
    if (auto gpu = fields.optionalObject("gpuMilliseconds")) {
        stats.shadowGpuMilliseconds = gpu->optionalNumber("shadow");
        stats.depthGpuMilliseconds = gpu->optionalNumber("depth");
        stats.ssaoGpuMilliseconds = gpu->optionalNumber("ssao");
        stats.sceneGpuMilliseconds = gpu->optionalNumber("scene");
        stats.postGpuMilliseconds = gpu->optionalNumber("post");
    }
    return stats;
}

/// A finite double, or null for an absent one (left out of the JSON by the caller).
void putOptional(Json& json, const char* key, const std::optional<double>& value) {
    if (value && std::isfinite(*value)) {
        json[key] = *value;
    }
}

Json timesToJson(const TimeSummary& times) {
    return Json{{"count", times.count},
                {"p50Ms", times.p50Ms},
                {"p95Ms", times.p95Ms},
                {"p99Ms", times.p99Ms},
                {"maxMs", times.maxMs}};
}

TimeSummary readTimes(FieldReader& fields) {
    return TimeSummary{fields.count("count"), fields.number("p50Ms"), fields.number("p95Ms"), fields.number("p99Ms"),
                       fields.number("maxMs")};
}

/// "count" and, when it is not 0, the percentiles of `times` into `json` (a frame-time record or a phase).
void putTimes(Json& json, const std::optional<TimeSummary>& times) {
    if (!times || times->count == 0) {
        json["count"] = 0;
        return;
    }
    const Json percentiles = timesToJson(*times);
    for (const auto& [key, value] : percentiles.items()) {
        json[key] = value;
    }
}

/// What putTimes() wrote: nothing for a count of 0, else the percentiles, which must be there.
std::optional<TimeSummary> readOptionalTimes(FieldReader& fields) {
    if (fields.count("count") == 0) {
        return std::nullopt;
    }
    return readTimes(fields);
}

Json frameTimesToJson(const FrameTimesRecord& record) {
    Json json;
    json["warmUpFrames"] = record.warmUpFrames;
    putTimes(json, record.times);
    return json;
}

Json phaseToJson(const PhaseRecord& phase) {
    Json json;
    json["name"] = phase.name;
    json["opened"] = phase.opened;
    json["open"] = phase.open;
    json["frames"] = phase.frames;
    putTimes(json, phase.times);
    return json;
}

PhaseRecord readPhase(FieldReader& fields) {
    PhaseRecord phase;
    phase.name = fields.string("name");
    phase.opened = fields.count("opened");
    phase.open = fields.boolean("open");
    phase.frames = fields.count("frames");
    phase.times = readOptionalTimes(fields);
    return phase;
}

Json cpuSectionToJson(const CpuSectionRecord& section) {
    Json json;
    json["name"] = section.name;
    const Json percentiles = timesToJson(section.times);
    for (const auto& [key, value] : percentiles.items()) {
        json[key] = value;
    }
    return json;
}

Json gpuMemoryToJson(const GpuMemoryRecord& memory) {
    Json json;
    json["totalBytes"] = memory.totalBytes;
    json["peakTotalBytes"] = memory.peakTotalBytes;
    Json kinds = Json::object();
    for (const gl::GpuMemoryKind kind : gl::kGpuMemoryKinds) {
        const gl::GpuMemoryUsage& usage = memory.usage(kind);
        kinds[std::string(gl::toString(kind))] =
            Json{{"bytes", usage.bytes}, {"peakBytes", usage.peakBytes}, {"allocations", usage.allocations}};
    }
    json["kinds"] = std::move(kinds);
    return json;
}

/// Every kind this build knows must be there; kinds of a newer writer are ignored.
GpuMemoryRecord readGpuMemory(FieldReader& fields) {
    GpuMemoryRecord memory;
    memory.totalBytes = fields.count("totalBytes");
    memory.peakTotalBytes = fields.count("peakTotalBytes");
    if (auto kinds = fields.object("kinds")) {
        for (const gl::GpuMemoryKind kind : gl::kGpuMemoryKinds) {
            if (auto usage = kinds->object(gl::toString(kind))) {
                memory.kinds[static_cast<std::size_t>(kind)] =
                    gl::GpuMemoryUsage{usage->count("bytes"), usage->count("peakBytes"), usage->count("allocations")};
            }
        }
    }
    return memory;
}

Json spawnToJson(const SpawnPoint& pose) {
    Json camera;
    // Floats widened to double print with enough digits to read back exactly.
    camera["feet"] =
        Json::array({static_cast<double>(pose.feet.x), static_cast<double>(pose.feet.y), static_cast<double>(pose.feet.z)});
    if (pose.yawDeg) {
        camera["yawDeg"] = static_cast<double>(*pose.yawDeg);
    }
    if (pose.pitchDeg) {
        camera["pitchDeg"] = static_cast<double>(*pose.pitchDeg);
    }
    return camera;
}

SpawnPoint readSpawn(FieldReader& camera) {
    SpawnPoint pose;
    pose.feet = camera.vec3("feet");
    if (const auto yaw = camera.optionalNumber("yawDeg")) {
        pose.yawDeg = static_cast<float>(*yaw);
    }
    if (const auto pitch = camera.optionalNumber("pitchDeg")) {
        pose.pitchDeg = static_cast<float>(*pitch);
    }
    return pose;
}

Json viewToJson(const CatalogView& view) {
    Json json;
    json["selectedId"] = view.selectedId;
    if (view.selectedPosition) {
        json["selectedPosition"] = *view.selectedPosition;
    }
    json["readiness"] = view.readiness;
    json["blocker"] = view.blocker;
    json["search"] = view.search;
    json["filter"] = view.filter;
    if (view.camera) {
        json["camera"] = spawnToJson(*view.camera);
    }
    return json;
}

CatalogView readView(FieldReader& fields) {
    CatalogView view;
    view.selectedId = fields.string("selectedId");
    if (const auto position = fields.optionalCount("selectedPosition", kMaxSize)) {
        view.selectedPosition = static_cast<std::size_t>(*position);
    }
    view.readiness = fields.string("readiness");
    view.blocker = fields.string("blocker");
    view.search = fields.string("search");
    view.filter = fields.string("filter");
    if (auto camera = fields.optionalObject("camera")) {
        view.camera = readSpawn(*camera);
    }
    return view;
}

Json catalogToJson(const CatalogRecord& catalog) {
    Json json;
    json["source"] = catalog.source;
    json["file"] = core::pathToGenericUtf8(catalog.file);
    json["games"] = catalog.games;
    json["opened"] = catalog.opened;
    json["open"] = catalog.open;
    if (catalog.selectedPosition) {
        json["selectedPosition"] = *catalog.selectedPosition;
    }
    json["selectedId"] = catalog.selectedId;
    // I2 step 13 (keys only added: a report without them reads as before).
    json["notice"] = catalog.notice;
    json["library"] = catalog.library;
    json["systems"] = catalog.systems;
    json["search"] = catalog.search;
    json["filter"] = catalog.filter;
    json["listed"] = catalog.listed;
    if (catalog.shownCount) {
        json["shownCount"] = *catalog.shownCount;
    }
    Json times = Json::object();
    putOptional(times, "openedMs", catalog.openedMs);
    putOptional(times, "firstListMs", catalog.firstListMs);
    putOptional(times, "firstPageMs", catalog.firstPageMs);
    json["times"] = std::move(times);
    if (catalog.pages) {
        json["pages"] = timesToJson(*catalog.pages);
    }
    if (catalog.frames) {
        json["frames"] = timesToJson(*catalog.frames);
    }
    json["interruptedSessions"] = catalog.interruptedSessions;
    json["cardSqueezed"] = catalog.cardSqueezed;
    return json;
}

Json launchToJson(const LaunchRecord& launch) {
    Json json;
    json["gameId"] = launch.gameId;
    json["profile"] = launch.profile;
    json["state"] = launch.state;
    json["outcome"] = launch.outcome;
    json["reason"] = launch.reason;
    json["detail"] = launch.detail;
    if (launch.exitCode) {
        json["exitCode"] = *launch.exitCode;
    }
    json["strategy"] = launch.strategy;
    json["processId"] = launch.processId;
    json["quickExit"] = launch.quickExit;
    json["processesRemained"] = launch.processesRemained;
    json["confirmedByUser"] = launch.confirmedByUser;
    json["wasStillRunning"] = launch.wasStillRunning;
    json["quitDuringGame"] = launch.quitDuringGame;
    json["waitLimitReached"] = launch.waitLimitReached;
    Json frames;
    frames["request"] = launch.requestFrame;
    if (launch.startFrame) {
        frames["start"] = *launch.startFrame;
    }
    if (launch.endFrame) {
        frames["end"] = *launch.endFrame;
    }
    frames["withPanel"] = launch.framesWithPanel;
    frames["whileRunning"] = launch.framesWhileRunning;
    json["frames"] = std::move(frames);
    json["idleWakes"] = launch.idleWakes;
    Json times = Json::object();
    putOptional(times, "startMs", launch.startMs);
    putOptional(times, "runMs", launch.runMs);
    putOptional(times, "returnMs", launch.returnMs);
    putOptional(times, "foregroundMs", launch.foregroundMs);
    json["times"] = std::move(times);
    json["panels"] = launch.panels;
    if (launch.before) {
        json["before"] = viewToJson(*launch.before);
    }
    if (launch.after) {
        json["after"] = viewToJson(*launch.after);
    }
    if (launch.sessionId || !launch.sessionError.empty() || launch.verified) {
        Json session;
        if (launch.sessionId) {
            session["id"] = *launch.sessionId;
        }
        session["error"] = launch.sessionError;
        session["verified"] = launch.verified;
        json["session"] = std::move(session);
    }
    return json;
}

CatalogRecord readCatalog(FieldReader& fields) {
    CatalogRecord catalog;
    catalog.source = fields.string("source");
    catalog.file = core::pathFromUtf8(fields.string("file"));
    catalog.games = static_cast<std::size_t>(fields.count("games", kMaxSize));
    catalog.opened = fields.count("opened");
    catalog.open = fields.boolean("open");
    if (const auto position = fields.optionalCount("selectedPosition", kMaxSize)) {
        catalog.selectedPosition = static_cast<std::size_t>(*position);
    }
    catalog.selectedId = fields.string("selectedId");
    // Written since I2 step 13: optional, so that the reports of earlier builds read.
    catalog.notice = fields.optionalString("notice").value_or("");
    catalog.library = fields.optionalString("library").value_or("");
    catalog.systems = static_cast<std::size_t>(fields.optionalCount("systems", kMaxSize).value_or(0));
    catalog.search = fields.optionalString("search").value_or("");
    catalog.filter = fields.optionalString("filter").value_or("");
    // Written since the M1 manual session's fixes: optional as well.
    catalog.listed = static_cast<std::size_t>(fields.optionalCount("listed", kMaxSize).value_or(0));
    if (const auto shown = fields.optionalCount("shownCount", kMaxSize)) {
        catalog.shownCount = static_cast<std::size_t>(*shown);
    }
    if (auto times = fields.optionalObject("times")) {
        catalog.openedMs = times->optionalNumber("openedMs");
        catalog.firstListMs = times->optionalNumber("firstListMs");
        catalog.firstPageMs = times->optionalNumber("firstPageMs");
    }
    if (auto pages = fields.optionalObject("pages")) {
        catalog.pages = readTimes(*pages);
    }
    if (auto frames = fields.optionalObject("frames")) {
        catalog.frames = readTimes(*frames);
    }
    catalog.interruptedSessions =
        static_cast<std::size_t>(fields.optionalCount("interruptedSessions", kMaxSize).value_or(0));
    if (fields.has("cardSqueezed")) {
        catalog.cardSqueezed = fields.stringArray("cardSqueezed");
    }
    return catalog;
}

LaunchRecord readLaunch(FieldReader& fields) {
    constexpr std::uint64_t kMaxUint32 = std::numeric_limits<std::uint32_t>::max();
    LaunchRecord launch;
    launch.gameId = fields.string("gameId");
    launch.profile = fields.string("profile");
    launch.state = fields.string("state");
    launch.outcome = fields.string("outcome");
    launch.reason = fields.string("reason");
    launch.detail = fields.string("detail");
    if (const auto code = fields.optionalCount("exitCode", kMaxUint32)) {
        launch.exitCode = static_cast<std::uint32_t>(*code);
    }
    launch.strategy = fields.string("strategy");
    launch.processId = static_cast<std::uint32_t>(fields.count("processId", kMaxUint32));
    launch.quickExit = fields.boolean("quickExit");
    launch.processesRemained = fields.boolean("processesRemained");
    launch.confirmedByUser = fields.boolean("confirmedByUser");
    launch.wasStillRunning = fields.boolean("wasStillRunning");
    launch.quitDuringGame = fields.boolean("quitDuringGame");
    launch.waitLimitReached = fields.boolean("waitLimitReached");
    if (auto frames = fields.optionalObject("frames")) {
        launch.requestFrame = frames->count("request");
        launch.startFrame = frames->optionalCount("start");
        launch.endFrame = frames->optionalCount("end");
        launch.framesWithPanel = frames->count("withPanel");
        launch.framesWhileRunning = frames->count("whileRunning");
    } else {
        fields.fail("frames", "missing required key");
    }
    launch.idleWakes = fields.count("idleWakes");
    if (auto times = fields.optionalObject("times")) {
        launch.startMs = times->optionalNumber("startMs");
        launch.runMs = times->optionalNumber("runMs");
        launch.returnMs = times->optionalNumber("returnMs");
        launch.foregroundMs = times->optionalNumber("foregroundMs");
    } else {
        fields.fail("times", "missing required key");
    }
    launch.panels = fields.stringArray("panels");
    if (auto before = fields.optionalObject("before")) {
        launch.before = readView(*before);
    }
    if (auto after = fields.optionalObject("after")) {
        launch.after = readView(*after);
    }
    if (auto session = fields.optionalObject("session")) {
        if (const auto id = session->optionalCount("id", static_cast<std::uint64_t>(
                                                             std::numeric_limits<std::int64_t>::max()))) {
            launch.sessionId = static_cast<std::int64_t>(*id);
        }
        launch.sessionError = session->string("error");
        launch.verified = session->boolean("verified");
    }
    return launch;
}

} // namespace

std::optional<TimeSummary> summarizeTimes(std::vector<double>& milliseconds) {
    return core::summarizeDurations(milliseconds);
}

GpuMemoryRecord gpuMemoryRecord(const gl::GpuMemoryLedger& ledger) {
    GpuMemoryRecord memory;
    memory.totalBytes = ledger.totalBytes();
    memory.peakTotalBytes = ledger.peakTotalBytes();
    memory.kinds = ledger.kinds();
    return memory;
}

std::vector<CpuSectionRecord> cpuSectionRecords(const core::CpuTimers& timers) {
    std::vector<CpuSectionRecord> sections;
    for (core::CpuSectionSummary& summary : timers.summaries()) {
        sections.push_back(CpuSectionRecord{std::move(summary.name), summary.times});
    }
    return sections;
}

std::string formatRunReport(const RunReport& report) {
    Json json;
    json["schemaVersion"] = kRunReportSchemaVersion;
    json["application"] = report.application;
    json["version"] = report.version;
    json["buildType"] = report.buildType;
    json["developerOverlay"] = report.developerOverlay;
    json["glDebugOutput"] = report.glDebugOutput;
    json["assetRoot"] = core::pathToGenericUtf8(report.assetRoot);
    json["assetRootSource"] = report.assetRootSource;
    json["exitCode"] = report.exitCode;
    json["outcome"] = report.outcome;
    if (report.failure) {
        json["failure"] = *report.failure;
    }
    json["openGlUnavailable"] = report.openGlUnavailable;
    if (report.context) {
        json["context"] = Json{{"vendor", report.context->vendor},
                               {"renderer", report.context->renderer},
                               {"version", report.context->version},
                               {"glslVersion", report.context->glslVersion}};
    }
    if (report.debugMessages) {
        json["debugMessages"] = Json{{"high", report.debugMessages->high},
                                     {"medium", report.debugMessages->medium},
                                     {"low", report.debugMessages->low}};
    }
    if (report.glSelfTest) {
        json["glSelfTest"] = Json{{"errorFlagRaised", report.glSelfTest->errorFlagRaised},
                                  {"debugOutputActive", report.glSelfTest->debugOutputActive},
                                  {"messagesCaptured", report.glSelfTest->messagesCaptured},
                                  {"highSeverityCaptured", report.glSelfTest->highSeverityCaptured}};
    }
    Json frames;
    if (report.frameLimit) {
        frames["limit"] = *report.frameLimit;
    }
    frames["rendered"] = report.framesRendered;
    json["frames"] = std::move(frames);
    if (report.lastView) {
        json["lastView"] = frameStatsToJson(*report.lastView);
    }
    json["instanceCapacity"] = report.instanceCapacity;
    if (report.screenshot) {
        json["screenshot"] = Json{{"file", core::pathToGenericUtf8(report.screenshot->file)},
                                  {"width", report.screenshot->width},
                                  {"height", report.screenshot->height}};
    }
    if (report.finalCamera) {
        json["finalCamera"] = spawnToJson(*report.finalCamera);
    }
    if (report.selfCheck) {
        json["selfCheck"] = Json{{"name", std::string(toString(report.selfCheck->check))},
                                 {"passed", report.selfCheck->passed},
                                 {"summary", report.selfCheck->summary}};
    }
    if (report.catalog) {
        json["catalog"] = catalogToJson(*report.catalog);
    }
    // Only runs that launched something have the key: the reports of the other runs stay as they were.
    if (!report.launches.empty()) {
        Json launches = Json::array();
        for (const LaunchRecord& launch : report.launches) {
            launches.push_back(launchToJson(launch));
        }
        json["launches"] = std::move(launches);
    }
    // The sections of I3: always written; world, residency and video empty until their steps fill them.
    json["world"] = Json::object();
    json["residency"] = Json::object();
    json["video"] = Json::object();
    json["frameTimes"] = frameTimesToJson(report.frameTimes);
    Json phases = Json::array();
    for (const PhaseRecord& phase : report.phases) {
        phases.push_back(phaseToJson(phase));
    }
    json["phases"] = std::move(phases);
    Json sections = Json::array();
    for (const CpuSectionRecord& section : report.cpuSections) {
        sections.push_back(cpuSectionToJson(section));
    }
    json["cpuSections"] = std::move(sections);
    json["gpuMemory"] = gpuMemoryToJson(report.gpuMemory);
    return json.dump(2) + "\n";
}

Expected<RunReport> parseRunReport(std::string_view json, std::string_view context) {
    auto parsed = core::parseJson(json, context);
    if (!parsed) {
        return propagate(parsed.error());
    }
    std::optional<Error> error;
    FieldReader fields(*parsed, "", context, error);
    if (const int version = fields.integer("schemaVersion"); !error && version != kRunReportSchemaVersion) {
        fields.fail("schemaVersion", fmt::format("unsupported version {} (expected {})", version,
                                                 kRunReportSchemaVersion));
    }
    RunReport report;
    report.application = fields.string("application");
    report.version = fields.string("version");
    report.buildType = fields.string("buildType");
    report.developerOverlay = fields.boolean("developerOverlay");
    report.glDebugOutput = fields.boolean("glDebugOutput");
    report.assetRoot = core::pathFromUtf8(fields.string("assetRoot"));
    report.assetRootSource = fields.string("assetRootSource");
    report.exitCode = fields.integer("exitCode");
    report.outcome = fields.string("outcome");
    report.failure = fields.optionalString("failure");
    report.openGlUnavailable = fields.boolean("openGlUnavailable");
    if (auto gl = fields.optionalObject("context")) {
        report.context = RunReportContext{gl->string("vendor"), gl->string("renderer"), gl->string("version"),
                                          gl->string("glslVersion")};
    }
    if (auto messages = fields.optionalObject("debugMessages")) {
        report.debugMessages = DebugMessageCounts{messages->count("high"), messages->count("medium"),
                                                  messages->count("low")};
    }
    if (auto selfTest = fields.optionalObject("glSelfTest")) {
        SelfTestOutcome outcome;
        outcome.errorFlagRaised = selfTest->boolean("errorFlagRaised");
        outcome.debugOutputActive = selfTest->boolean("debugOutputActive");
        outcome.messagesCaptured = selfTest->count("messagesCaptured");
        outcome.highSeverityCaptured = selfTest->count("highSeverityCaptured", outcome.messagesCaptured);
        report.glSelfTest = outcome;
    }
    if (auto frames = fields.optionalObject("frames")) {
        if (const auto limit = frames->optionalCount("limit", std::numeric_limits<std::uint32_t>::max())) {
            report.frameLimit = static_cast<std::uint32_t>(*limit);
        }
        report.framesRendered = frames->count("rendered");
    } else {
        fields.fail("frames", "missing required key");
    }
    if (auto view = fields.optionalObject("lastView")) {
        report.lastView = readFrameStats(*view);
    }
    report.instanceCapacity = static_cast<std::size_t>(fields.count("instanceCapacity", kMaxSize));
    if (auto screenshot = fields.optionalObject("screenshot")) {
        const auto extent = static_cast<std::uint64_t>(std::numeric_limits<int>::max());
        report.screenshot = ScreenshotRecord{core::pathFromUtf8(screenshot->string("file")),
                                             static_cast<int>(screenshot->count("width", extent)),
                                             static_cast<int>(screenshot->count("height", extent))};
    }
    if (auto camera = fields.optionalObject("finalCamera")) {
        report.finalCamera = readSpawn(*camera);
    }
    if (auto check = fields.optionalObject("selfCheck")) {
        const std::string name = check->string("name");
        const std::optional<SelfCheck> known = selfCheckFromString(name);
        if (!known) {
            check->fail("name", "unknown self-check \"" + name + "\"");
        }
        report.selfCheck = SelfCheckRecord{known.value_or(SelfCheck::Srgb), check->boolean("passed"),
                                           check->string("summary")};
    }
    if (auto catalog = fields.optionalObject("catalog")) {
        report.catalog = readCatalog(*catalog);
    }
    for (FieldReader& launch : fields.optionalObjectArray("launches")) {
        report.launches.push_back(readLaunch(launch));
    }
    // The sections of I3 must be there; world, residency and video are empty until their steps fill them.
    static_cast<void>(fields.object("world"));
    static_cast<void>(fields.object("residency"));
    static_cast<void>(fields.object("video"));
    if (auto frameTimes = fields.object("frameTimes")) {
        report.frameTimes.warmUpFrames = frameTimes->count("warmUpFrames");
        report.frameTimes.times = readOptionalTimes(*frameTimes);
    }
    for (FieldReader& phase : fields.objectArray("phases")) {
        report.phases.push_back(readPhase(phase));
    }
    for (FieldReader& section : fields.objectArray("cpuSections")) {
        report.cpuSections.push_back(CpuSectionRecord{section.string("name"), readTimes(section)});
    }
    if (auto memory = fields.object("gpuMemory")) {
        report.gpuMemory = readGpuMemory(*memory);
    }
    if (error) {
        return propagate(std::move(*error));
    }
    return report;
}

Status writeRunReport(const std::filesystem::path& path, const RunReport& report) {
    return core::writeTextFileAtomic(path, formatRunReport(report));
}

Expected<RunReport> readRunReport(const std::filesystem::path& path) {
    auto text = core::readTextFile(path);
    if (!text) {
        return propagate(text.error());
    }
    return parseRunReport(*text, core::pathToUtf8(path));
}

} // namespace rv::app
