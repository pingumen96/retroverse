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
    // The static instance groups and the outline (I3 step 8): left out of views without them, so that the reports of
    // the reference scenes read as before.
    if (stats.staticGroups > 0) {
        json["staticGroups"] = Json{{"groups", stats.staticGroups},
                                    {"batches", stats.staticBatches},
                                    {"clusters", stats.clusters},
                                    {"culledClusters", stats.culledClusters},
                                    {"drawCalls", stats.staticDrawCalls},
                                    {"instances", stats.staticInstances}};
    }
    if (stats.outlinedInstances > 0) {
        json["outlinedInstances"] = stats.outlinedInstances;
    }
    Json gpu = Json::object();
    const std::pair<const char*, const std::optional<double>*> times[] = {
        {"shadow", &stats.shadowGpuMilliseconds}, {"depth", &stats.depthGpuMilliseconds},
        {"ssao", &stats.ssaoGpuMilliseconds},     {"scene", &stats.sceneGpuMilliseconds},
        {"post", &stats.postGpuMilliseconds},     {"outline", &stats.outlineGpuMilliseconds},
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
    /// Four numbers (I10 step 11: a crop's corner and size).
    [[nodiscard]] glm::vec4 vec4(std::string_view key) {
        const nlohmann::json* value = required(key);
        if (value == nullptr) {
            return glm::vec4(0.0f);
        }
        if (!value->is_array() || value->size() != 4 ||
            !std::all_of(value->begin(), value->end(), [](const nlohmann::json& item) { return item.is_number(); })) {
            fail(key, "expected an array of four numbers");
            return glm::vec4(0.0f);
        }
        return glm::vec4((*value)[0].get<float>(), (*value)[1].get<float>(), (*value)[2].get<float>(),
                         (*value)[3].get<float>());
    }

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

    /// An array of strings; none when the key is absent.
    [[nodiscard]] std::vector<std::string> optionalStringArray(std::string_view key) {
        if (find(key) == nullptr) {
            return {};
        }
        return stringArray(key);
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

    /// An array of finite numbers.
    [[nodiscard]] std::vector<double> numberArray(std::string_view key) {
        std::vector<double> numbers;
        const nlohmann::json* value = required(key);
        if (value == nullptr) {
            return numbers;
        }
        if (!value->is_array()) {
            fail(key, "expected an array of numbers");
            return numbers;
        }
        for (const nlohmann::json& item : *value) {
            if (!item.is_number() || !std::isfinite(item.get<double>())) {
                fail(key, "expected an array of finite numbers");
                return {};
            }
            numbers.push_back(item.get<double>());
        }
        return numbers;
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
    if (auto groups = fields.optionalObject("staticGroups")) {
        stats.staticGroups = groups->count("groups", kMaxSize);
        stats.staticBatches = groups->count("batches", kMaxSize);
        stats.clusters = groups->count("clusters", kMaxSize);
        stats.culledClusters = groups->count("culledClusters", kMaxSize);
        stats.staticDrawCalls = groups->count("drawCalls", kMaxSize);
        stats.staticInstances = groups->count("instances", kMaxSize);
    }
    stats.outlinedInstances = fields.optionalCount("outlinedInstances", kMaxSize).value_or(0);
    if (auto gpu = fields.optionalObject("gpuMilliseconds")) {
        stats.shadowGpuMilliseconds = gpu->optionalNumber("shadow");
        stats.depthGpuMilliseconds = gpu->optionalNumber("depth");
        stats.ssaoGpuMilliseconds = gpu->optionalNumber("ssao");
        stats.sceneGpuMilliseconds = gpu->optionalNumber("scene");
        stats.postGpuMilliseconds = gpu->optionalNumber("post");
        stats.outlineGpuMilliseconds = gpu->optionalNumber("outline");
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

/// "gpuPasses" (I3 step 17b) into `json`, left out when there is none.
void putGpuPasses(Json& json, const std::vector<GpuPassRecord>& passes) {
    if (passes.empty()) {
        return;
    }
    Json array = Json::array();
    for (const GpuPassRecord& pass : passes) {
        Json entry;
        entry["name"] = pass.name;
        const Json times = timesToJson(pass.times);
        for (const auto& [key, value] : times.items()) {
            entry[key] = value;
        }
        array.push_back(std::move(entry));
    }
    json["gpuPasses"] = std::move(array);
}

/// What putGpuPasses() wrote (none for a report without the key: written before I3 step 17b, or without a view).
std::vector<GpuPassRecord> readGpuPasses(FieldReader& fields) {
    std::vector<GpuPassRecord> passes;
    for (FieldReader& pass : fields.optionalObjectArray("gpuPasses")) {
        passes.push_back(GpuPassRecord{pass.string("name"), readTimes(pass)});
    }
    return passes;
}

Json frameTimesToJson(const FrameTimesRecord& record) {
    Json json;
    json["warmUpFrames"] = record.warmUpFrames;
    putTimes(json, record.times);
    putGpuPasses(json, record.gpuPasses);
    return json;
}

Json phaseToJson(const PhaseRecord& phase) {
    Json json;
    json["name"] = phase.name;
    json["opened"] = phase.opened;
    json["open"] = phase.open;
    json["frames"] = phase.frames;
    putTimes(json, phase.times);
    putGpuPasses(json, phase.gpuPasses);
    return json;
}

PhaseRecord readPhase(FieldReader& fields) {
    PhaseRecord phase;
    phase.name = fields.string("name");
    phase.opened = fields.count("opened");
    phase.open = fields.boolean("open");
    phase.frames = fields.count("frames");
    phase.times = readOptionalTimes(fields);
    phase.gpuPasses = readGpuPasses(fields);
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

/// An unbound record (no collection) is an empty object.
Json vec3ToJson(const glm::vec3& value) {
    return Json::array({static_cast<double>(value.x), static_cast<double>(value.y), static_cast<double>(value.z)});
}

Json inspectToJson(const InspectRecord& inspect) {
    Json json{{"inspections", inspect.inspections},
              {"liftFrames", inspect.liftFrames},
              {"heldFrames", inspect.heldFrames},
              {"dimmedFrames", inspect.dimmedFrames},
              {"mediaRequests", inspect.mediaRequests},
              {"infoRequests", inspect.infoRequests},
              {"catalogOpenedInInspect", inspect.catalogOpenedInInspect},
              {"pickId", inspect.pickId},
              {"lift", inspect.lift},
              {"yawDeg", inspect.yawDeg},
              {"pitchDeg", inspect.pitchDeg},
              {"distance", inspect.distance},
              {"drawnDistance", inspect.drawnDistance},
              {"heldCenter", vec3ToJson(inspect.heldCenter)},
              {"heldFront", vec3ToJson(inspect.heldFront)},
              {"heldUp", vec3ToJson(inspect.heldUp)},
              {"heroFront", inspect.heroFront}};
    putOptional(json, "minDistance", inspect.minDistance);
    putOptional(json, "maxDistance", inspect.maxDistance);
    putOptional(json, "minDrawnDistance", inspect.minDrawnDistance);
    putOptional(json, "maxDrawnDistance", inspect.maxDrawnDistance);
    return json;
}

InspectRecord readInspect(FieldReader& fields) {
    InspectRecord inspect;
    inspect.inspections = fields.count("inspections");
    inspect.liftFrames = fields.count("liftFrames");
    inspect.heldFrames = fields.count("heldFrames");
    inspect.dimmedFrames = fields.count("dimmedFrames");
    inspect.mediaRequests = fields.count("mediaRequests");
    inspect.infoRequests = fields.count("infoRequests");
    inspect.catalogOpenedInInspect = fields.count("catalogOpenedInInspect");
    inspect.minDistance = fields.optionalNumber("minDistance");
    inspect.maxDistance = fields.optionalNumber("maxDistance");
    inspect.minDrawnDistance = fields.optionalNumber("minDrawnDistance");
    inspect.maxDrawnDistance = fields.optionalNumber("maxDrawnDistance");
    inspect.pickId = fields.string("pickId");
    inspect.lift = fields.number("lift");
    inspect.yawDeg = fields.number("yawDeg");
    inspect.pitchDeg = fields.number("pitchDeg");
    inspect.distance = fields.number("distance");
    inspect.drawnDistance = fields.number("drawnDistance");
    inspect.heldCenter = fields.vec3("heldCenter");
    inspect.heldFront = fields.vec3("heldFront");
    inspect.heldUp = fields.vec3("heldUp");
    inspect.heroFront = fields.boolean("heroFront");
    return inspect;
}

Json insertionToJson(const InsertionRecord& insertion) {
    Json json{{"started", insertion.started},
              {"arrived", insertion.arrived},
              {"skipped", insertion.skipped},
              {"refused", insertion.refused},
              {"movingFrames", insertion.movingFrames},
              {"socketFrames", insertion.socketFrames},
              {"pickId", insertion.pickId},
              {"seconds", insertion.seconds},
              {"end", insertion.end},
              {"socket", vec3ToJson(insertion.socket)},
              {"center", vec3ToJson(insertion.center)},
              {"scale", insertion.scale},
              {"distance", insertion.distance}};
    if (insertion.startFrame) {
        json["startFrame"] = *insertion.startFrame;
    }
    if (insertion.seatedFrame) {
        json["seatedFrame"] = *insertion.seatedFrame;
    }
    if (insertion.endFrame) {
        json["endFrame"] = *insertion.endFrame;
    }
    Json frames = Json::array();
    for (const InsertionFrameRecord& frame : insertion.frames) {
        frames.push_back(Json{{"frame", frame.frame}, {"progress", frame.progress}, {"distance", frame.distance}});
    }
    json["frames"] = std::move(frames);
    return json;
}

InsertionRecord readInsertion(FieldReader& fields) {
    InsertionRecord insertion;
    insertion.started = fields.count("started");
    insertion.arrived = fields.count("arrived");
    insertion.skipped = fields.count("skipped");
    insertion.refused = fields.count("refused");
    insertion.movingFrames = fields.count("movingFrames");
    insertion.socketFrames = fields.count("socketFrames");
    insertion.pickId = fields.string("pickId");
    insertion.startFrame = fields.optionalCount("startFrame");
    insertion.seatedFrame = fields.optionalCount("seatedFrame");
    insertion.endFrame = fields.optionalCount("endFrame");
    insertion.seconds = fields.number("seconds");
    insertion.end = fields.string("end");
    insertion.socket = fields.vec3("socket");
    insertion.center = fields.vec3("center");
    insertion.scale = fields.number("scale");
    insertion.distance = fields.number("distance");
    for (FieldReader& frame : fields.objectArray("frames")) {
        insertion.frames.push_back(
            InsertionFrameRecord{frame.count("frame"), frame.number("progress"), frame.number("distance")});
    }
    return insertion;
}

Json interactionToJson(const InteractionRecord& interaction) {
    Json json = Json::object();
    if (!interaction.bound) {
        return json;
    }
    json["bound"] = true;
    json["reduceMotion"] = interaction.reduceMotion;
    json["state"] = interaction.state;
    json["hoveredPickId"] = interaction.hoveredPickId;
    json["focusedPickId"] = interaction.focusedPickId;
    json["highlightedPickId"] = interaction.highlightedPickId;
    json["focusedGame"] = interaction.focusedGame;
    json["pulled"] = interaction.pulled;
    json["hoverChanges"] = interaction.hoverChanges;
    json["focusChanges"] = interaction.focusChanges;
    json["steps"] = interaction.steps;
    json["pullOuts"] = interaction.pullOuts;
    json["pulledFrames"] = interaction.pulledFrames;
    json["maxPulled"] = interaction.maxPulled;
    json["staticDrawnWhilePulledFrames"] = interaction.staticDrawnWhilePulledFrames;
    json["animationFrames"] = interaction.animationFrames;
    json["walkerEaseFrames"] = interaction.walkerEaseFrames;
    json["cursorFrames"] = interaction.cursorFrames;
    json["labelFrames"] = interaction.labelFrames;
    json["label"] = interaction.label;
    Json events = Json::array();
    for (const InteractionEventRecord& event : interaction.events) {
        Json entry{{"frame", event.frame}, {"event", event.event}};
        if (!event.pickId.empty()) {
            entry["pickId"] = event.pickId;
        }
        if (!event.state.empty()) {
            entry["state"] = event.state;
        }
        events.push_back(std::move(entry));
    }
    json["events"] = std::move(events);
    json["eventsDropped"] = interaction.eventsDropped;
    json["inspect"] = inspectToJson(interaction.inspect);
    json["insertion"] = insertionToJson(interaction.insertion);
    return json;
}

InteractionRecord readInteraction(FieldReader& fields) {
    InteractionRecord interaction;
    if (!fields.has("bound")) {
        return interaction;
    }
    interaction.bound = fields.boolean("bound");
    interaction.reduceMotion = fields.boolean("reduceMotion");
    interaction.state = fields.string("state");
    interaction.hoveredPickId = fields.string("hoveredPickId");
    interaction.focusedPickId = fields.string("focusedPickId");
    interaction.highlightedPickId = fields.string("highlightedPickId");
    interaction.focusedGame = fields.string("focusedGame");
    interaction.pulled = fields.stringArray("pulled");
    interaction.hoverChanges = fields.count("hoverChanges");
    interaction.focusChanges = fields.count("focusChanges");
    interaction.steps = fields.count("steps");
    interaction.pullOuts = fields.count("pullOuts");
    interaction.pulledFrames = fields.count("pulledFrames");
    interaction.maxPulled = fields.count("maxPulled");
    interaction.staticDrawnWhilePulledFrames = fields.count("staticDrawnWhilePulledFrames");
    interaction.animationFrames = fields.count("animationFrames");
    interaction.walkerEaseFrames = fields.count("walkerEaseFrames");
    interaction.cursorFrames = fields.count("cursorFrames");
    interaction.labelFrames = fields.count("labelFrames");
    interaction.label = fields.string("label");
    for (FieldReader& event : fields.objectArray("events")) {
        InteractionEventRecord entry;
        entry.frame = event.count("frame");
        entry.event = event.string("event");
        entry.pickId = event.optionalString("pickId").value_or(std::string());
        entry.state = event.optionalString("state").value_or(std::string());
        interaction.events.push_back(std::move(entry));
    }
    interaction.eventsDropped = fields.count("eventsDropped");
    if (std::optional<FieldReader> inspect = fields.optionalObject("inspect")) {
        interaction.inspect = readInspect(*inspect);
    }
    if (std::optional<FieldReader> insertion = fields.optionalObject("insertion")) {
        interaction.insertion = readInsertion(*insertion);
    }
    return interaction;
}

/// An unbound record (no collection) is an empty object.
Json cardToJson(const CardRecord& card) {
    Json json = Json::object();
    if (!card.bound) {
        return json;
    }
    json["bound"] = true;
    json["shownFrames"] = card.shownFrames;
    json["coverFrames"] = card.coverFrames;
    json["squeezedFrames"] = card.squeezedFrames;
    json["game"] = card.game;
    json["title"] = card.title;
    json["platform"] = card.platform;
    json["year"] = card.year;
    json["readiness"] = card.readiness;
    json["blocker"] = card.blocker;
    json["details"] = card.details;
    json["otherVersions"] = card.otherVersions;
    json["focused"] = card.focused;
    json["coverBox"] = card.coverBox;
    Json events = Json::array();
    for (const CardEventRecord& event : card.events) {
        Json entry{{"frame", event.frame}, {"event", event.event}};
        for (const auto& [key, value] : {std::pair<const char*, const std::string*>{"button", &event.button},
                                         {"via", &event.via},
                                         {"game", &event.game}}) {
            if (!value->empty()) {
                entry[key] = *value;
            }
        }
        events.push_back(std::move(entry));
    }
    json["events"] = std::move(events);
    json["eventsDropped"] = card.eventsDropped;
    return json;
}

CardRecord readCard(FieldReader& fields) {
    CardRecord card;
    if (!fields.has("bound")) {
        return card;
    }
    card.bound = fields.boolean("bound");
    card.shownFrames = fields.count("shownFrames");
    card.coverFrames = fields.count("coverFrames");
    card.squeezedFrames = fields.count("squeezedFrames");
    card.game = fields.string("game");
    card.title = fields.string("title");
    card.platform = fields.string("platform");
    card.year = fields.count("year");
    card.readiness = fields.string("readiness");
    card.blocker = fields.string("blocker");
    card.details = fields.boolean("details");
    card.otherVersions = fields.count("otherVersions");
    card.focused = fields.string("focused");
    card.coverBox = fields.numberArray("coverBox");
    for (FieldReader& event : fields.objectArray("events")) {
        CardEventRecord entry;
        entry.frame = event.count("frame");
        entry.event = event.string("event");
        entry.button = event.optionalString("button").value_or(std::string());
        entry.via = event.optionalString("via").value_or(std::string());
        entry.game = event.optionalString("game").value_or(std::string());
        card.events.push_back(std::move(entry));
    }
    card.eventsDropped = fields.count("eventsDropped");
    return card;
}

Json spawnToJson(const SpawnPoint& pose);
SpawnPoint readSpawn(FieldReader& camera);

/// An unbound record (no plan shown) is an empty object (I3 step 15a).
Json roomLaunchToJson(const RoomLaunchRecord& launch) {
    Json json = Json::object();
    if (!launch.bound) {
        return json;
    }
    json["bound"] = true;
    json["launches"] = launch.launches;
    json["returned"] = launch.returned;
    json["notReturned"] = launch.notReturned;
    json["showInRoom"] = launch.showInRoom;
    json["showInRoomFailed"] = launch.showInRoomFailed;
    if (launch.walkerBefore) {
        json["walkerBefore"] = spawnToJson(*launch.walkerBefore);
    }
    if (launch.walkerAfter) {
        json["walkerAfter"] = spawnToJson(*launch.walkerAfter);
    }
    json["poseRestored"] = launch.poseRestored;
    json["placementQueries"] = launch.placementQueries;
    json["placementGame"] = launch.placementGame;
    json["placement"] = launch.placement;
    Json events = Json::array();
    for (const RoomLaunchEventRecord& event : launch.events) {
        Json entry{{"frame", event.frame}, {"event", event.event}};
        for (const auto& [key, value] : {std::pair<const char*, const std::string*>{"game", &event.game},
                                         {"pickId", &event.pickId},
                                         {"outcome", &event.outcome},
                                         {"reason", &event.reason},
                                         {"state", &event.state}}) {
            if (!value->empty()) {
                entry[key] = *value;
            }
        }
        if (event.walker) {
            entry["walker"] = spawnToJson(*event.walker);
        }
        events.push_back(std::move(entry));
    }
    json["events"] = std::move(events);
    json["eventsDropped"] = launch.eventsDropped;
    return json;
}

RoomLaunchRecord readRoomLaunch(FieldReader& fields) {
    RoomLaunchRecord launch;
    if (!fields.has("bound")) {
        return launch;
    }
    launch.bound = fields.boolean("bound");
    launch.launches = fields.count("launches");
    launch.returned = fields.count("returned");
    launch.notReturned = fields.count("notReturned");
    launch.showInRoom = fields.count("showInRoom");
    launch.showInRoomFailed = fields.count("showInRoomFailed");
    if (auto before = fields.optionalObject("walkerBefore")) {
        launch.walkerBefore = readSpawn(*before);
    }
    if (auto after = fields.optionalObject("walkerAfter")) {
        launch.walkerAfter = readSpawn(*after);
    }
    launch.poseRestored = fields.boolean("poseRestored");
    launch.placementQueries = fields.count("placementQueries");
    launch.placementGame = fields.string("placementGame");
    launch.placement = fields.string("placement");
    for (FieldReader& event : fields.objectArray("events")) {
        RoomLaunchEventRecord entry;
        entry.frame = event.count("frame");
        entry.event = event.string("event");
        entry.game = event.optionalString("game").value_or(std::string());
        entry.pickId = event.optionalString("pickId").value_or(std::string());
        entry.outcome = event.optionalString("outcome").value_or(std::string());
        entry.reason = event.optionalString("reason").value_or(std::string());
        entry.state = event.optionalString("state").value_or(std::string());
        if (auto walker = event.optionalObject("walker")) {
            entry.walker = readSpawn(*walker);
        }
        launch.events.push_back(std::move(entry));
    }
    launch.eventsDropped = fields.count("eventsDropped");
    return launch;
}

/// An unbound record (no product UI) is an empty object (I3 step 14b).
Json screensToJson(const GameScreensRecord& screens) {
    Json json = Json::object();
    if (!screens.bound) {
        return json;
    }
    json["bound"] = true;
    json["viewerFrames"] = screens.viewerFrames;
    json["infoFrames"] = screens.infoFrames;
    json["pictures"] = screens.pictures;
    json["failures"] = screens.failures;
    json["pictureBox"] = screens.pictureBox;
    json["stageBox"] = screens.stageBox;
    json["videoFrames"] = screens.videoFrames;
    json["videoBox"] = screens.videoBox;
    Json events = Json::array();
    for (const GameScreenEventRecord& event : screens.events) {
        Json entry{{"frame", event.frame}, {"event", event.event}};
        for (const auto& [key, value] : {std::pair<const char*, const std::string*>{"game", &event.game},
                                         {"from", &event.from},
                                         {"kind", &event.kind},
                                         {"detail", &event.detail}}) {
            if (!value->empty()) {
                entry[key] = *value;
            }
        }
        events.push_back(std::move(entry));
    }
    json["events"] = std::move(events);
    json["eventsDropped"] = screens.eventsDropped;
    return json;
}

GameScreensRecord readScreens(FieldReader& fields) {
    GameScreensRecord screens;
    if (!fields.has("bound")) {
        return screens;
    }
    screens.bound = fields.boolean("bound");
    screens.viewerFrames = fields.count("viewerFrames");
    screens.infoFrames = fields.count("infoFrames");
    screens.pictures = fields.count("pictures");
    screens.failures = fields.count("failures");
    screens.pictureBox = fields.numberArray("pictureBox");
    screens.stageBox = fields.numberArray("stageBox");
    // I5 step 8 (absent from earlier reports).
    screens.videoFrames = fields.optionalCount("videoFrames").value_or(0);
    if (fields.has("videoBox")) {
        screens.videoBox = fields.numberArray("videoBox");
    }
    for (FieldReader& event : fields.objectArray("events")) {
        GameScreenEventRecord entry;
        entry.frame = event.count("frame");
        entry.event = event.string("event");
        entry.game = event.optionalString("game").value_or(std::string());
        entry.from = event.optionalString("from").value_or(std::string());
        entry.kind = event.optionalString("kind").value_or(std::string());
        entry.detail = event.optionalString("detail").value_or(std::string());
        screens.events.push_back(std::move(entry));
    }
    screens.eventsDropped = fields.count("eventsDropped");
    return screens;
}

/// An unbound record (no generated building) is an empty object (I4 step 7b).
Json directoryToJson(const DirectoryRecord& directory) {
    Json json = Json::object();
    if (!directory.bound) {
        return json;
    }
    json["bound"] = true;
    json["opened"] = directory.opened;
    json["open"] = directory.open;
    json["tab"] = directory.tab;
    json["view"] = directory.view;
    json["notice"] = directory.notice;
    json["gamesState"] = directory.gamesState;
    json["gamesRequests"] = directory.gamesRequests;
    Json games = Json::array();
    for (const DirectoryGameRecord& game : directory.games) {
        games.push_back(Json{{"gameId", game.gameId},
                             {"title", game.title},
                             {"system", game.system},
                             {"favorite", game.favorite},
                             {"played", game.played}});
    }
    json["games"] = std::move(games);
    json["cardGame"] = directory.cardGame;
    json["cardRoom"] = directory.cardRoom;
    json["cardOutcome"] = directory.cardOutcome;
    Json events = Json::array();
    for (const DirectoryEventRecord& event : directory.events) {
        Json entry{{"frame", event.frame}, {"event", event.event}};
        if (!event.id.empty()) {
            entry["id"] = event.id;
        }
        if (!event.detail.empty()) {
            entry["detail"] = event.detail;
        }
        events.push_back(std::move(entry));
    }
    json["events"] = std::move(events);
    json["eventsDropped"] = directory.eventsDropped;
    Json chainRooms = Json::array();
    for (const DirectoryChainRoomRecord& room : directory.chainRooms) {
        Json ranges = Json::array();
        for (const DirectoryRangeRecord& range : room.ranges) {
            ranges.push_back(Json{{"system", range.system}, {"first", range.first}, {"last", range.last}});
        }
        chainRooms.push_back(Json{{"room", room.room}, {"ranges", std::move(ranges)}, {"scattered", room.scattered}});
    }
    json["chainRooms"] = std::move(chainRooms);
    return json;
}

DirectoryRecord readDirectory(FieldReader& fields) {
    DirectoryRecord directory;
    if (!fields.has("bound")) {
        return directory;
    }
    directory.bound = fields.boolean("bound");
    directory.opened = fields.count("opened");
    directory.open = fields.boolean("open");
    directory.tab = fields.string("tab");
    directory.view = fields.string("view");
    directory.notice = fields.string("notice");
    directory.gamesState = fields.string("gamesState");
    directory.gamesRequests = fields.count("gamesRequests");
    for (FieldReader& game : fields.objectArray("games")) {
        DirectoryGameRecord entry;
        entry.gameId = game.string("gameId");
        entry.title = game.string("title");
        entry.system = game.string("system");
        entry.favorite = game.boolean("favorite");
        entry.played = game.string("played");
        directory.games.push_back(std::move(entry));
    }
    directory.cardGame = fields.string("cardGame");
    directory.cardRoom = fields.string("cardRoom");
    directory.cardOutcome = fields.string("cardOutcome");
    for (FieldReader& event : fields.objectArray("events")) {
        DirectoryEventRecord entry;
        entry.frame = event.count("frame");
        entry.event = event.string("event");
        entry.id = event.optionalString("id").value_or(std::string());
        entry.detail = event.optionalString("detail").value_or(std::string());
        directory.events.push_back(std::move(entry));
    }
    directory.eventsDropped = fields.count("eventsDropped");
    // I4b step 8: a report written before it lists no room of a chain.
    for (FieldReader& room : fields.optionalObjectArray("chainRooms")) {
        DirectoryChainRoomRecord entry;
        entry.room = room.string("room");
        for (FieldReader& range : room.objectArray("ranges")) {
            entry.ranges.push_back(
                DirectoryRangeRecord{range.string("system"), range.string("first"), range.string("last")});
        }
        entry.scattered = room.boolean("scattered");
        directory.chainRooms.push_back(std::move(entry));
    }
    return directory;
}

Json menuToJson(const MenuRecord& menu) {
    Json json{{"opened", menu.opened},
              {"settingsOpened", menu.settingsOpened},
              {"open", menu.open},
              {"view", menu.view},
              {"settingsOpen", menu.settingsOpen},
              {"settingsGroup", menu.settingsGroup},
              {"settingsRows", menu.settingsRows},
              {"quit", menu.quit},
              {"textCut", menu.textCut},
              {"settingsTextCut", menu.settingsTextCut},
              {"settingsRowCovered", menu.settingsRowCovered}};
    Json events = Json::array();
    for (const MenuEventRecord& event : menu.events) {
        Json entry{{"frame", event.frame}, {"event", event.event}};
        if (!event.detail.empty()) {
            entry["detail"] = event.detail;
        }
        events.push_back(std::move(entry));
    }
    json["events"] = std::move(events);
    json["eventsDropped"] = menu.eventsDropped;
    return json;
}

MenuRecord readMenu(FieldReader& fields) {
    MenuRecord menu;
    menu.opened = fields.count("opened");
    menu.settingsOpened = fields.count("settingsOpened");
    menu.open = fields.boolean("open");
    menu.view = fields.string("view");
    menu.settingsOpen = fields.boolean("settingsOpen");
    menu.settingsGroup = fields.string("settingsGroup");
    menu.settingsRows = fields.stringArray("settingsRows");
    menu.quit = fields.boolean("quit");
    menu.textCut = static_cast<std::uint32_t>(fields.count("textCut"));
    menu.settingsTextCut = static_cast<std::uint32_t>(fields.count("settingsTextCut"));
    menu.settingsRowCovered = fields.boolean("settingsRowCovered");
    for (FieldReader& event : fields.objectArray("events")) {
        MenuEventRecord entry;
        entry.frame = event.count("frame");
        entry.event = event.string("event");
        entry.detail = event.optionalString("detail").value_or(std::string());
        menu.events.push_back(std::move(entry));
    }
    menu.eventsDropped = fields.count("eventsDropped");
    return menu;
}

Json wizardToJson(const WizardRecord& wizard) {
    Json json{{"resumed", wizard.resumed},
              {"open", wizard.open},
              {"page", wizard.page},
              {"detectionWork", wizard.detectionWork},
              {"previewWork", wizard.previewWork},
              {"importWork", wizard.importWork},
              {"worldWork", wizard.worldWork},
              {"problem", wizard.problem},
              {"failure", wizard.failure},
              {"entered", wizard.entered},
              {"pages", wizard.pages},
              {"systems", wizard.systems},
              {"systemsWithGames", wizard.systemsWithGames},
              {"entries", wizard.entries},
              {"games", wizard.games},
              {"esdeGames", wizard.esdeGames},
              {"missingFiles", wizard.missingFiles},
              {"notAGame", wizard.notAGame},
              {"discMembers", wizard.discMembers},
              {"media", wizard.media},
              {"issues", wizard.issues},
              {"errors", wizard.errors},
              {"added", wizard.added},
              {"browsed", wizard.browsed},
              {"browserState", wizard.browserState},
              {"browserFailure", wizard.browserFailure},
              {"textCut", wizard.textCut},
              {"browserTextCut", wizard.browserTextCut}};
    Json events = Json::array();
    for (const WizardEventRecord& event : wizard.events) {
        events.push_back(Json{{"frame", event.frame}, {"event", event.event}, {"detail", event.detail}});
    }
    json["events"] = std::move(events);
    json["eventsDropped"] = wizard.eventsDropped;
    return json;
}

WizardRecord readWizard(FieldReader& fields) {
    WizardRecord wizard;
    wizard.resumed = fields.boolean("resumed");
    wizard.open = fields.boolean("open");
    wizard.page = fields.string("page");
    wizard.detectionWork = fields.string("detectionWork");
    wizard.previewWork = fields.string("previewWork");
    wizard.importWork = fields.string("importWork");
    wizard.worldWork = fields.string("worldWork");
    wizard.problem = fields.string("problem");
    wizard.failure = fields.string("failure");
    wizard.entered = fields.boolean("entered");
    wizard.pages = fields.stringArray("pages");
    wizard.systems = fields.count("systems");
    wizard.systemsWithGames = fields.count("systemsWithGames");
    wizard.entries = fields.count("entries");
    wizard.games = fields.count("games");
    wizard.esdeGames = fields.count("esdeGames");
    wizard.missingFiles = fields.count("missingFiles");
    wizard.notAGame = fields.count("notAGame");
    wizard.discMembers = fields.count("discMembers");
    wizard.media = fields.count("media");
    wizard.issues = fields.count("issues");
    wizard.errors = fields.count("errors");
    wizard.added = fields.count("added");
    wizard.browsed = fields.count("browsed");
    wizard.browserState = fields.string("browserState");
    wizard.browserFailure = fields.string("browserFailure");
    wizard.textCut = static_cast<std::uint32_t>(fields.count("textCut"));
    wizard.browserTextCut = static_cast<std::uint32_t>(fields.count("browserTextCut"));
    for (FieldReader& event : fields.objectArray("events")) {
        WizardEventRecord entry;
        entry.frame = event.count("frame");
        entry.event = event.string("event");
        entry.detail = event.string("detail");
        wizard.events.push_back(std::move(entry));
    }
    wizard.eventsDropped = fields.count("eventsDropped");
    return wizard;
}

Json controlsToJson(const ControlsRecord& controls) {
    Json hints = Json::array();
    for (const HintRecord& hint : controls.hints) {
        hints.push_back(Json{{"actions", hint.actions}, {"text", hint.text}});
    }
    return Json{{"device", controls.device},           {"safeBindings", controls.safeBindings},
                {"hints", std::move(hints)},           {"hintsOutside", controls.hintsOutside},
                {"captures", controls.captures},       {"padsLost", controls.padsLost},
                {"padsBack", controls.padsBack},       {"noticeShown", controls.noticeShown},
                {"noticeFrames", controls.noticeFrames}, {"bindings", controls.bindings}};
}

ControlsRecord readControls(FieldReader& fields) {
    ControlsRecord controls;
    controls.device = fields.string("device");
    controls.safeBindings = fields.boolean("safeBindings");
    for (FieldReader& hint : fields.objectArray("hints")) {
        controls.hints.push_back(HintRecord{hint.string("actions"), hint.string("text")});
    }
    controls.hintsOutside = static_cast<std::uint32_t>(fields.count("hintsOutside"));
    controls.captures = fields.stringArray("captures");
    controls.padsLost = fields.count("padsLost");
    controls.padsBack = fields.count("padsBack");
    controls.noticeShown = fields.boolean("noticeShown");
    controls.noticeFrames = fields.count("noticeFrames");
    controls.bindings = fields.stringArray("bindings");
    return controls;
}

Json settingsToJson(const SettingsRecord& settings) {
    Json json{{"unreadable", settings.unreadable},
              {"backedUp", settings.backedUp},
              {"shadowed", settings.shadowed},
              {"refused", settings.refused},
              {"writes", settings.writes},
              {"writeFailures", settings.writeFailures}};
    Json rows = Json::array();
    for (const SettingValueRecord& row : settings.rows) {
        rows.push_back(Json{{"key", row.key}, {"value", row.value}, {"overridden", row.overridden}});
    }
    json["rows"] = std::move(rows);
    Json changes = Json::array();
    for (const SettingChangeRecord& change : settings.changes) {
        changes.push_back(Json{
            {"frame", change.frame}, {"kind", change.kind}, {"key", change.key}, {"value", change.value}});
    }
    json["changes"] = std::move(changes);
    json["changesDropped"] = settings.changesDropped;
    json["lensFovDeg"] = settings.lensFovDeg;
    json["mouseSensitivity"] = settings.mouseSensitivity;
    json["stickSensitivity"] = settings.stickSensitivity;
    json["invertY"] = settings.invertY;
    json["textScale"] = settings.textScale;
    json["displayMode"] = settings.displayMode;
    json["windowFullscreen"] = settings.windowFullscreen;
    json["displayModeApplied"] = settings.displayModeApplied;
    json["reduceMotion"] = settings.reduceMotion;
    json["masterGain"] = settings.masterGain;
    return json;
}

SettingsRecord readSettings(FieldReader& fields) {
    SettingsRecord settings;
    settings.unreadable = fields.boolean("unreadable");
    settings.backedUp = fields.boolean("backedUp");
    settings.shadowed = fields.stringArray("shadowed");
    settings.refused = fields.stringArray("refused");
    settings.writes = fields.count("writes");
    settings.writeFailures = fields.count("writeFailures");
    for (FieldReader& row : fields.objectArray("rows")) {
        SettingValueRecord entry;
        entry.key = row.string("key");
        entry.value = row.string("value");
        entry.overridden = row.boolean("overridden");
        settings.rows.push_back(std::move(entry));
    }
    for (FieldReader& change : fields.objectArray("changes")) {
        SettingChangeRecord entry;
        entry.frame = change.count("frame");
        entry.kind = change.string("kind");
        entry.key = change.string("key");
        entry.value = change.string("value");
        settings.changes.push_back(std::move(entry));
    }
    settings.changesDropped = fields.count("changesDropped");
    settings.lensFovDeg = fields.number("lensFovDeg");
    settings.mouseSensitivity = fields.number("mouseSensitivity");
    settings.stickSensitivity = fields.number("stickSensitivity");
    settings.invertY = fields.boolean("invertY");
    settings.textScale = fields.number("textScale");
    settings.displayMode = fields.string("displayMode");
    settings.windowFullscreen = fields.boolean("windowFullscreen");
    settings.displayModeApplied = fields.count("displayModeApplied");
    settings.reduceMotion = fields.boolean("reduceMotion");
    settings.masterGain = fields.number("masterGain");
    return settings;
}

/// An unbound record (no room asked for) is the empty object it was before I3 step 10b.
Json roomUpdatesToJson(const std::vector<RoomUpdateRecord>& rooms) {
    Json array = Json::array();
    for (const RoomUpdateRecord& room : rooms) {
        array.push_back(Json{{"room", room.room},
                             {"kind", room.kind},
                             {"kept", room.kept},
                             {"moved", room.moved},
                             {"flagsChanged", room.flagsChanged},
                             {"added", room.added},
                             {"removed", room.removed},
                             {"coversChanged", room.coversChanged}});
    }
    return array;
}

std::vector<RoomUpdateRecord> readRoomUpdates(FieldReader& fields) {
    std::vector<RoomUpdateRecord> rooms;
    for (FieldReader& room : fields.optionalObjectArray("rooms")) {
        RoomUpdateRecord record;
        record.room = room.string("room");
        record.kind = room.string("kind");
        record.kept = room.count("kept");
        record.moved = room.count("moved");
        record.flagsChanged = room.count("flagsChanged");
        record.added = room.count("added");
        record.removed = room.count("removed");
        record.coversChanged = room.count("coversChanged");
        rooms.push_back(std::move(record));
    }
    return rooms;
}

void putWalker(Json& json, const std::optional<WalkerKeptRecord>& walker) {
    if (walker) {
        json["walker"] = Json{{"area", walker->area},
                              {"shift", Json::array({static_cast<double>(walker->shift.x),
                                                     static_cast<double>(walker->shift.y),
                                                     static_cast<double>(walker->shift.z)})},
                              {"halfTurn", walker->halfTurn}};
    }
}

std::optional<WalkerKeptRecord> readWalker(FieldReader& fields) {
    auto walker = fields.optionalObject("walker");
    if (!walker) {
        return std::nullopt;
    }
    WalkerKeptRecord record;
    record.area = walker->string("area");
    record.shift = walker->vec3("shift");
    record.halfTurn = walker->boolean("halfTurn");
    return record;
}

/// I4b step 3: the editor's sessions (world.edits).
Json editsToJson(const std::vector<WorldEditRecord>& edits) {
    Json sessions = Json::array();
    for (const WorldEditRecord& edit : edits) {
        Json operations = Json::array();
        for (const EditOperationRecord& operation : edit.operations) {
            Json record{{"frame", operation.frame},
                        {"op", operation.op},
                        {"operation", operation.operation},
                        {"refused", operation.refused},
                        {"rebuild", operation.rebuild},
                        {"path", operation.path}};
            if (operation.shownFrame) {
                record["shownFrame"] = *operation.shownFrame;
            }
            putOptional(record, "editMs", operation.editMs);
            putOptional(record, "shownMs", operation.shownMs);
            operations.push_back(std::move(record));
        }
        Json session{{"room", edit.room},
                     {"via", edit.via},
                     {"enterFrame", edit.enterFrame},
                     {"heldAtStart", edit.heldAtStart},
                     {"failed", edit.failed},
                     {"operations", std::move(operations)},
                     {"undone", edit.undone},
                     {"menus", edit.menus},
                     {"end", edit.end},
                     {"written", edit.written},
                     {"replayed", edit.replayed},
                     {"saveRefused", edit.saveRefused},
                     {"snapshot", edit.snapshot},
                     {"planDigest", edit.planDigest},
                     {"liveUpdateDeferred", edit.liveUpdateDeferred},
                     {"probes", edit.probes},
                     {"rooms", edit.rooms}};
        if (edit.readyFrame) {
            session["readyFrame"] = *edit.readyFrame;
        }
        if (edit.endFrame) {
            session["endFrame"] = *edit.endFrame;
        }
        putOptional(session, "prepareMs", edit.prepareMs);
        putOptional(session, "saveMs", edit.saveMs);
        putOptional(session, "backMs", edit.backMs);
        sessions.push_back(std::move(session));
    }
    return sessions;
}

std::vector<WorldEditRecord> readEdits(FieldReader& fields) {
    std::vector<WorldEditRecord> edits;
    for (FieldReader& session : fields.optionalObjectArray("edits")) {
        WorldEditRecord edit;
        edit.room = session.string("room");
        edit.via = session.string("via");
        edit.enterFrame = session.count("enterFrame");
        edit.heldAtStart = session.string("heldAtStart");
        edit.readyFrame = session.optionalCount("readyFrame");
        edit.prepareMs = session.optionalNumber("prepareMs");
        edit.failed = session.string("failed");
        for (FieldReader& operation : session.optionalObjectArray("operations")) {
            EditOperationRecord record;
            record.frame = operation.count("frame");
            record.shownFrame = operation.optionalCount("shownFrame");
            record.op = operation.string("op");
            record.operation = operation.string("operation");
            record.refused = operation.string("refused");
            record.rebuild = operation.string("rebuild");
            record.path = operation.string("path");
            record.editMs = operation.optionalNumber("editMs");
            record.shownMs = operation.optionalNumber("shownMs");
            edit.operations.push_back(std::move(record));
        }
        edit.undone = session.count("undone");
        edit.menus = session.count("menus");
        edit.end = session.string("end");
        edit.endFrame = session.optionalCount("endFrame");
        edit.written = session.boolean("written");
        edit.replayed = session.boolean("replayed");
        edit.saveRefused = session.string("saveRefused");
        edit.snapshot = session.string("snapshot");
        edit.planDigest = session.string("planDigest");
        edit.saveMs = session.optionalNumber("saveMs");
        edit.backMs = session.optionalNumber("backMs");
        edit.liveUpdateDeferred = session.boolean("liveUpdateDeferred");
        edit.probes = session.optionalCount("probes").value_or(0);
        edit.rooms = session.optionalStringArray("rooms");
        edits.push_back(std::move(edit));
    }
    return edits;
}

/// I4b step 6: the regenerations and undos (world.regenerations).
Json regenerationsToJson(const std::vector<WorldRegenerationRecord>& regenerations) {
    Json records = Json::array();
    for (const WorldRegenerationRecord& regeneration : regenerations) {
        Json record{{"kind", regeneration.kind},
                    {"scope", regeneration.scope},
                    {"via", regeneration.via},
                    {"requestFrame", regeneration.requestFrame},
                    {"inputsTaken", regeneration.inputsTaken},
                    {"failure", regeneration.failure},
                    {"roomsRegenerated", regeneration.roomsRegenerated},
                    {"roomsAdded", regeneration.roomsAdded},
                    {"roomsRemoved", regeneration.roomsRemoved},
                    {"roomsMoved", regeneration.roomsMoved},
                    {"gamesMoved", regeneration.gamesMoved},
                    {"faceOutEntering", regeneration.faceOutEntering},
                    {"faceOutLeaving", regeneration.faceOutLeaving},
                    {"newlyPlaced", regeneration.newlyPlaced},
                    {"newlyUnplaced", regeneration.newlyUnplaced},
                    {"fixturesAdded", regeneration.fixturesAdded},
                    {"fixturesRemoved", regeneration.fixturesRemoved},
                    {"fixturesMoved", regeneration.fixturesMoved},
                    {"pinsKept", regeneration.pinsKept},
                    {"conflicts", regeneration.conflicts},
                    {"overrides", regeneration.overrides},
                    {"changed", regeneration.changed},
                    {"undoSnapshot", regeneration.undoSnapshot},
                    {"confirmations", regeneration.confirmations},
                    {"choice", regeneration.choice},
                    {"written", regeneration.written},
                    {"reused", regeneration.reused},
                    {"snapshot", regeneration.snapshot},
                    {"planDigest", regeneration.planDigest},
                    {"path", regeneration.path},
                    {"walker", regeneration.walker},
                    {"end", regeneration.end}};
        if (regeneration.previewFrame) {
            record["previewFrame"] = *regeneration.previewFrame;
        }
        if (regeneration.choiceFrame) {
            record["choiceFrame"] = *regeneration.choiceFrame;
        }
        if (regeneration.backFrame) {
            record["backFrame"] = *regeneration.backFrame;
        }
        putOptional(record, "previewMs", regeneration.previewMs);
        putOptional(record, "previewJobMs", regeneration.previewJobMs);
        putOptional(record, "inputsMs", regeneration.inputsMs);
        putOptional(record, "regenerateMs", regeneration.regenerateMs);
        putOptional(record, "commitMs", regeneration.commitMs);
        putOptional(record, "backMs", regeneration.backMs);
        records.push_back(std::move(record));
    }
    return records;
}

std::vector<WorldRegenerationRecord> readRegenerations(FieldReader& fields) {
    std::vector<WorldRegenerationRecord> regenerations;
    for (FieldReader& record : fields.optionalObjectArray("regenerations")) {
        WorldRegenerationRecord regeneration;
        regeneration.kind = record.string("kind");
        regeneration.scope = record.string("scope");
        regeneration.via = record.string("via");
        regeneration.requestFrame = record.count("requestFrame");
        regeneration.previewFrame = record.optionalCount("previewFrame");
        regeneration.previewMs = record.optionalNumber("previewMs");
        regeneration.previewJobMs = record.optionalNumber("previewJobMs");
        regeneration.inputsMs = record.optionalNumber("inputsMs");
        regeneration.inputsTaken = record.boolean("inputsTaken");
        regeneration.regenerateMs = record.optionalNumber("regenerateMs");
        regeneration.failure = record.string("failure");
        regeneration.roomsRegenerated = record.count("roomsRegenerated");
        regeneration.roomsAdded = record.count("roomsAdded");
        regeneration.roomsRemoved = record.count("roomsRemoved");
        regeneration.roomsMoved = record.count("roomsMoved");
        regeneration.gamesMoved = record.count("gamesMoved");
        regeneration.faceOutEntering = record.count("faceOutEntering");
        regeneration.faceOutLeaving = record.count("faceOutLeaving");
        regeneration.newlyPlaced = record.count("newlyPlaced");
        regeneration.newlyUnplaced = record.count("newlyUnplaced");
        regeneration.fixturesAdded = record.count("fixturesAdded");
        regeneration.fixturesRemoved = record.count("fixturesRemoved");
        regeneration.fixturesMoved = record.count("fixturesMoved");
        regeneration.pinsKept = record.count("pinsKept");
        regeneration.conflicts = record.count("conflicts");
        regeneration.overrides = record.count("overrides");
        regeneration.changed = record.boolean("changed");
        regeneration.undoSnapshot = record.string("undoSnapshot");
        regeneration.confirmations = record.count("confirmations");
        regeneration.choice = record.string("choice");
        regeneration.choiceFrame = record.optionalCount("choiceFrame");
        regeneration.written = record.boolean("written");
        regeneration.reused = record.boolean("reused");
        regeneration.snapshot = record.string("snapshot");
        regeneration.planDigest = record.string("planDigest");
        regeneration.commitMs = record.optionalNumber("commitMs");
        regeneration.backFrame = record.optionalCount("backFrame");
        regeneration.backMs = record.optionalNumber("backMs");
        regeneration.path = record.string("path");
        regeneration.walker = record.string("walker");
        regeneration.end = record.string("end");
        regenerations.push_back(std::move(regeneration));
    }
    return regenerations;
}

Json worldToJson(const WorldRecord& world) {
    Json json = Json::object();
    if (!world.bound) {
        return json;
    }
    json["bound"] = true;
    json["state"] = world.state;
    json["notice"] = world.notice;
    json["reason"] = world.reason;
    json["noticeRoom"] = world.noticeRoom;
    json["catalogState"] = world.catalogState;
    json["room"] = world.room;
    json["screenRoom"] = world.screenRoom;
    json["library"] = world.library;
    json["floor"] = world.floor;
    Json rooms = Json::array();
    for (const WorldRoomRecord& room : world.rooms) {
        rooms.push_back(Json{{"id", room.id},
                             {"floor", room.floor},
                             {"x", room.xMm},
                             {"z", room.zMm},
                             {"yaw", room.yawHalfTurns}});
    }
    json["rooms"] = std::move(rooms);
    json["outcome"] = world.outcome;
    json["changeOutcome"] = world.changeOutcome;
    json["rebuild"] = world.rebuild;
    json["planDigest"] = world.planDigest;
    json["planWritten"] = world.planWritten;
    json["saveError"] = world.saveError;
    json["placements"] = Json{{"total", world.placements},
                              {"featured", world.featured},
                              {"band", world.band},
                              {"spine", world.spine}};
    json["unplaced"] = Json{{"total", world.unplaced},
                            {"duplicateTitle", world.duplicateTitle},
                            {"noCover", world.noCover},
                            {"unavailable", world.unavailable},
                            {"capacity", world.capacity},
                            {"roomKept", world.roomKept}};
    json["added"] = world.added;
    json["kept"] = world.kept;
    Json migrated = Json::array();
    for (const MigratedRoomRecord& room : world.migratedRooms) {
        migrated.push_back(Json{{"room", room.room},
                                {"from", room.from},
                                {"to", room.to},
                                {"unplacedForCapacity", room.unplacedForCapacity}});
    }
    json["migratedRooms"] = std::move(migrated);
    Json replaced = Json::array();
    for (const ReplacedRoomRecord& room : world.replacedRooms) {
        replaced.push_back(Json{{"room", room.room},
                                {"fromXMm", room.fromXMm},
                                {"fromZMm", room.fromZMm},
                                {"toXMm", room.toXMm},
                                {"toZMm", room.toZMm}});
    }
    json["replacedRooms"] = std::move(replaced);
    json["needsConfirmation"] = world.needsConfirmation;
    json["roomsAdded"] = world.roomsAdded;
    json["hardFindings"] = world.hardFindings;
    json["softFindings"] = world.softFindings;
    json["objects"] = world.objects;
    json["lights"] = world.lights;
    json["undressed"] = world.undressed;
    json["items"] = world.items;
    json["clusters"] = world.clusters;
    json["skipped"] = world.skipped;
    json["staticRecords"] = world.staticRecords;
    json["pickTargets"] = world.pickTargets;
    Json times = Json::object();
    putOptional(times, "snapshotAtMs", world.snapshotAtMs);
    putOptional(times, "committedAtMs", world.committedAtMs);
    putOptional(times, "snapshotMs", world.snapshotMs);
    putOptional(times, "buildMs", world.buildMs);
    putOptional(times, "commitMs", world.commitMs);
    putOptional(times, "snapshotToCommitMs", world.snapshotToCommitMs);
    if (world.job) {
        times["job"] = Json{{"lockMs", world.job->lockMs},     {"selectMs", world.job->selectMs},
                            {"dataMs", world.job->dataMs},     {"planMs", world.job->planMs},
                            {"saveMs", world.job->saveMs},     {"itemsMs", world.job->itemsMs},
                            {"floorMs", world.job->floorMs},   {"totalMs", world.job->totalMs},
                            {"skipped", world.job->skipped}};
    }
    json["times"] = std::move(times);
    if (world.firstFrame) {
        json["firstFrame"] = *world.firstFrame;
    }
    json["hoverChanges"] = world.hoverChanges;
    json["hoveredPickId"] = world.hoveredPickId;
    json["hoveredGame"] = world.hoveredGame;
    json["interaction"] = interactionToJson(world.interaction);
    json["card"] = cardToJson(world.card);
    json["launch"] = roomLaunchToJson(world.launch);
    if (world.preview) {
        const WorldPreviewRecord& preview = *world.preview;
        Json record{{"frame", preview.frame},
                    {"planDigest", preview.planDigest},
                    {"items", preview.items},
                    {"itemsKnown", preview.itemsKnown}};
        putOptional(record, "committedAtMs", preview.committedAtMs);
        putOptional(record, "buildMs", preview.buildMs);
        putOptional(record, "commitMs", preview.commitMs);
        if (preview.frontsResident) {
            record["frontsResident"] = *preview.frontsResident;
        }
        if (preview.spinesResident) {
            record["spinesResident"] = *preview.spinesResident;
        }
        json["preview"] = std::move(record);
    }
    if (world.reconcile) {
        const WorldReconcileRecord& reconcile = *world.reconcile;
        Json record{{"kind", reconcile.kind},
                    {"frame", reconcile.frame},
                    {"kept", reconcile.kept},
                    {"moved", reconcile.moved},
                    {"flagsChanged", reconcile.flagsChanged},
                    {"added", reconcile.added},
                    {"removed", reconcile.removed},
                    {"coversReloaded", reconcile.coversReloaded},
                    {"rooms", roomUpdatesToJson(reconcile.rooms)}};
        putWalker(record, reconcile.walker);
        json["reconcile"] = std::move(record);
    }
    json["itemsWritten"] = world.itemsWritten;
    json["screenGames"] = world.screenGames;
    Json fallbacks = Json::array();
    for (const ObjectFallbackRecord& fallback : world.objectFallbacks) {
        fallbacks.push_back(
            Json{{"prefab", fallback.prefab}, {"source", fallback.source}, {"reason", fallback.reason}});
    }
    json["objectFallbacks"] = std::move(fallbacks);
    Json live = Json::array();
    for (const WorldLiveUpdateRecord& update : world.liveUpdates) {
        Json record{{"trigger", update.trigger},
                    {"requestFrame", update.requestFrame},
                    {"frame", update.frame},
                    {"kind", update.kind},
                    {"reason", update.reason},
                    {"outcome", update.outcome},
                    {"rebuild", update.rebuild},
                    {"planWritten", update.planWritten},
                    {"kept", update.kept},
                    {"moved", update.moved},
                    {"flagsChanged", update.flagsChanged},
                    {"added", update.added},
                    {"removed", update.removed},
                    {"coversChanged", update.coversChanged},
                    {"arraysGrown", update.arraysGrown},
                    {"arrayAdded", update.arrayAdded},
                    {"items", update.items},
                    {"staticRecords", update.staticRecords}};
        putOptional(record, "requestToApplyMs", update.requestToApplyMs);
        putOptional(record, "buildMs", update.buildMs);
        putOptional(record, "applyMs", update.applyMs);
        record["rooms"] = roomUpdatesToJson(update.rooms);
        putWalker(record, update.walker);
        live.push_back(std::move(record));
    }
    json["liveUpdates"] = std::move(live);
    // I4 step 7b: the floors, the rides, the teleports, the elevator's prompt.
    Json floors = Json::array();
    for (const WorldFloorRecord& floor : world.floors) {
        floors.push_back(
            Json{{"id", floor.id}, {"order", floor.order}, {"rooms", floor.rooms}, {"placed", floor.placed}});
    }
    json["floors"] = std::move(floors);
    Json rides = Json::array();
    for (const RideRecord& ride : world.rides) {
        Json record{{"from", ride.from},
                    {"to", ride.to},
                    {"reason", ride.reason},
                    {"room", ride.room},
                    {"game", ride.game},
                    {"requestedFrame", ride.requestedFrame},
                    {"outcome", ride.outcome},
                    {"arrayBytes", ride.arrayBytes},
                    {"gpuBytes", ride.gpuBytes}};
        if (ride.fadedOutFrame) {
            record["fadedOutFrame"] = *ride.fadedOutFrame;
        }
        if (ride.swappedFrame) {
            record["swappedFrame"] = *ride.swappedFrame;
        }
        if (ride.fadedInFrame) {
            record["fadedInFrame"] = *ride.fadedInFrame;
        }
        putOptional(record, "builtMs", ride.builtMs);
        putOptional(record, "residentMs", ride.residentMs);
        putOptional(record, "totalMs", ride.totalMs);
        putOptional(record, "jobMs", ride.jobMs);
        if (ride.viewItems) {
            record["viewItems"] = *ride.viewItems;
        }
        if (ride.viewPending) {
            record["viewPending"] = *ride.viewPending;
        }
        putOptional(record, "roomResidentMs", ride.roomResidentMs);
        if (ride.walker) {
            record["walker"] = spawnToJson(*ride.walker);
        }
        Json kinds = Json::object();
        for (const gl::GpuMemoryKind kind : gl::kGpuMemoryKinds) {
            kinds[std::string(gl::toString(kind))] = ride.bytesOf(kind);
        }
        record["gpuKinds"] = std::move(kinds);
        rides.push_back(std::move(record));
    }
    json["rides"] = std::move(rides);
    Json teleports = Json::array();
    for (const TeleportRecord& teleport : world.teleports) {
        teleports.push_back(Json{{"frame", teleport.frame},
                                 {"to", teleport.to},
                                 {"reason", teleport.reason},
                                 {"walker", spawnToJson(teleport.walker)}});
    }
    json["teleports"] = std::move(teleports);
    json["elevatorPromptFrames"] = world.elevatorPromptFrames;
    json["totemPromptFrames"] = world.totemPromptFrames;
    if (world.showcase.bound) {
        Json draws = Json::array();
        for (const ShowcaseDrawRecord& draw : world.showcase.draws) {
            draws.push_back(Json{{"meshes", draw.meshes}, {"materials", draw.materials}, {"inPlace", draw.inPlace}});
        }
        json["showcase"] = Json{{"games", world.showcase.games},
                                {"builds", world.showcase.builds},
                                {"unchanged", world.showcase.unchanged},
                                {"draws", std::move(draws)},
                                {"cardsOpened", world.showcase.cardsOpened},
                                {"lastCard", world.showcase.lastCard},
                                {"hovered", world.showcase.hovered},
                                {"frontsResident", world.showcase.frontsResident}};
    }
    if (!world.edits.empty()) {
        json["edits"] = editsToJson(world.edits);
    }
    if (!world.regenerations.empty()) {
        json["regenerations"] = regenerationsToJson(world.regenerations); // I4b step 6
    }
    json["regenerationInputsReleased"] = world.regenerationInputsReleased;
    json["cabinetClipFrames"] = world.cabinetClipFrames;
    json["cabinetCoverBeneathFrames"] = world.cabinetCoverBeneathFrames;
    return json;
}

WorldRecord readWorld(FieldReader& fields) {
    WorldRecord world;
    if (!fields.has("bound")) {
        return world;
    }
    world.bound = fields.boolean("bound");
    world.state = fields.string("state");
    world.notice = fields.string("notice");
    world.reason = fields.string("reason");
    world.noticeRoom = fields.optionalString("noticeRoom").value_or(std::string());
    world.catalogState = fields.string("catalogState");
    world.room = fields.string("room");
    world.screenRoom = fields.optionalString("screenRoom").value_or(std::string());
    world.library = fields.string("library");
    world.floor = fields.optionalString("floor").value_or(std::string());
    for (FieldReader& room : fields.optionalObjectArray("rooms")) {
        WorldRoomRecord record;
        record.id = room.string("id");
        record.floor = room.string("floor");
        record.xMm = room.integer("x");
        record.zMm = room.integer("z");
        record.yawHalfTurns = static_cast<std::uint32_t>(room.count("yaw", 1));
        world.rooms.push_back(std::move(record));
    }
    world.outcome = fields.string("outcome");
    world.changeOutcome = fields.string("changeOutcome");
    world.rebuild = fields.string("rebuild");
    world.planDigest = fields.string("planDigest");
    world.planWritten = fields.boolean("planWritten");
    world.saveError = fields.string("saveError");
    if (auto placements = fields.object("placements")) {
        world.placements = placements->count("total");
        world.featured = placements->count("featured");
        world.band = placements->count("band");
        world.spine = placements->count("spine");
    }
    if (auto unplaced = fields.object("unplaced")) {
        world.unplaced = unplaced->count("total");
        world.duplicateTitle = unplaced->count("duplicateTitle");
        world.noCover = unplaced->count("noCover");
        world.unavailable = unplaced->count("unavailable");
        world.capacity = unplaced->count("capacity");
        world.roomKept = unplaced->count("roomKept");
    }
    world.added = fields.count("added");
    world.kept = fields.count("kept");
    for (FieldReader& room : fields.optionalObjectArray("migratedRooms")) {
        MigratedRoomRecord record;
        record.room = room.string("room");
        record.from = room.string("from");
        record.to = room.string("to");
        record.unplacedForCapacity = room.optionalCount("unplacedForCapacity").value_or(0);
        world.migratedRooms.push_back(std::move(record));
    }
    for (FieldReader& room : fields.optionalObjectArray("replacedRooms")) {
        ReplacedRoomRecord record;
        record.room = room.string("room");
        record.fromXMm = room.integer("fromXMm");
        record.fromZMm = room.integer("fromZMm");
        record.toXMm = room.integer("toXMm");
        record.toZMm = room.integer("toZMm");
        world.replacedRooms.push_back(std::move(record));
    }
    world.needsConfirmation = fields.optionalStringArray("needsConfirmation");
    world.roomsAdded = fields.optionalStringArray("roomsAdded");
    world.hardFindings = fields.count("hardFindings");
    world.softFindings = fields.count("softFindings");
    world.objects = fields.count("objects");
    world.lights = fields.count("lights");
    world.undressed = fields.count("undressed");
    world.items = fields.count("items");
    world.clusters = fields.count("clusters");
    world.skipped = fields.count("skipped");
    world.staticRecords = fields.count("staticRecords");
    world.pickTargets = fields.count("pickTargets");
    if (auto times = fields.object("times")) {
        world.snapshotAtMs = times->optionalNumber("snapshotAtMs");
        world.committedAtMs = times->optionalNumber("committedAtMs");
        world.snapshotMs = times->optionalNumber("snapshotMs");
        world.buildMs = times->optionalNumber("buildMs");
        world.commitMs = times->optionalNumber("commitMs");
        world.snapshotToCommitMs = times->optionalNumber("snapshotToCommitMs");
        if (auto job = times->optionalObject("job")) {
            WorldJobRecord record;
            record.lockMs = job->number("lockMs");
            record.selectMs = job->number("selectMs");
            record.dataMs = job->number("dataMs");
            record.planMs = job->number("planMs");
            record.saveMs = job->number("saveMs");
            record.itemsMs = job->number("itemsMs");
            record.floorMs = job->number("floorMs");
            record.totalMs = job->number("totalMs");
            record.skipped = job->boolean("skipped");
            world.job = record;
        }
    }
    world.firstFrame = fields.optionalCount("firstFrame");
    world.hoverChanges = fields.count("hoverChanges");
    world.hoveredPickId = fields.string("hoveredPickId");
    world.hoveredGame = fields.string("hoveredGame");
    if (auto interaction = fields.optionalObject("interaction")) {
        world.interaction = readInteraction(*interaction);
    }
    if (auto launch = fields.optionalObject("launch")) {
        world.launch = readRoomLaunch(*launch);
    }
    if (auto card = fields.optionalObject("card")) {
        world.card = readCard(*card);
    }
    if (auto preview = fields.optionalObject("preview")) {
        WorldPreviewRecord record;
        record.frame = preview->count("frame");
        record.planDigest = preview->string("planDigest");
        record.items = preview->count("items");
        record.itemsKnown = preview->count("itemsKnown");
        record.committedAtMs = preview->optionalNumber("committedAtMs");
        record.buildMs = preview->optionalNumber("buildMs");
        record.commitMs = preview->optionalNumber("commitMs");
        record.frontsResident = preview->optionalCount("frontsResident");
        record.spinesResident = preview->optionalCount("spinesResident");
        world.preview = std::move(record);
    }
    if (auto reconcile = fields.optionalObject("reconcile")) {
        WorldReconcileRecord record;
        record.kind = reconcile->string("kind");
        record.frame = reconcile->count("frame");
        record.kept = reconcile->count("kept");
        record.moved = reconcile->count("moved");
        record.flagsChanged = reconcile->count("flagsChanged");
        record.added = reconcile->count("added");
        record.removed = reconcile->count("removed");
        record.coversReloaded = reconcile->boolean("coversReloaded");
        record.rooms = readRoomUpdates(*reconcile);
        record.walker = readWalker(*reconcile);
        world.reconcile = std::move(record);
    }
    world.itemsWritten = fields.boolean("itemsWritten");
    world.screenGames = fields.count("screenGames");
    for (FieldReader& fallback : fields.objectArray("objectFallbacks")) {
        world.objectFallbacks.push_back(
            ObjectFallbackRecord{fallback.string("prefab"), fallback.string("source"), fallback.string("reason")});
    }
    for (FieldReader& update : fields.optionalObjectArray("liveUpdates")) {
        WorldLiveUpdateRecord record;
        record.trigger = update.string("trigger");
        record.requestFrame = update.count("requestFrame");
        record.frame = update.count("frame");
        record.kind = update.string("kind");
        record.reason = update.string("reason");
        record.outcome = update.string("outcome");
        record.rebuild = update.string("rebuild");
        record.planWritten = update.boolean("planWritten");
        record.kept = update.count("kept");
        record.moved = update.count("moved");
        record.flagsChanged = update.count("flagsChanged");
        record.added = update.count("added");
        record.removed = update.count("removed");
        record.coversChanged = update.count("coversChanged");
        record.arraysGrown = update.boolean("arraysGrown");
        record.arrayAdded = update.boolean("arrayAdded");
        record.items = update.count("items");
        record.staticRecords = update.count("staticRecords");
        record.requestToApplyMs = update.optionalNumber("requestToApplyMs");
        record.buildMs = update.optionalNumber("buildMs");
        record.applyMs = update.optionalNumber("applyMs");
        record.rooms = readRoomUpdates(update);
        record.walker = readWalker(update);
        world.liveUpdates.push_back(std::move(record));
    }
    for (FieldReader& floor : fields.optionalObjectArray("floors")) {
        WorldFloorRecord record;
        record.id = floor.string("id");
        record.order = static_cast<std::uint32_t>(floor.count("order"));
        record.rooms = floor.count("rooms");
        record.placed = floor.count("placed");
        world.floors.push_back(std::move(record));
    }
    for (FieldReader& ride : fields.optionalObjectArray("rides")) {
        RideRecord record;
        record.from = ride.string("from");
        record.to = ride.string("to");
        record.reason = ride.string("reason");
        record.room = ride.string("room");
        record.game = ride.string("game");
        record.requestedFrame = ride.count("requestedFrame");
        record.fadedOutFrame = ride.optionalCount("fadedOutFrame");
        record.swappedFrame = ride.optionalCount("swappedFrame");
        record.fadedInFrame = ride.optionalCount("fadedInFrame");
        record.builtMs = ride.optionalNumber("builtMs");
        record.residentMs = ride.optionalNumber("residentMs");
        record.totalMs = ride.optionalNumber("totalMs");
        record.jobMs = ride.optionalNumber("jobMs");
        record.viewItems = ride.optionalCount("viewItems");
        record.viewPending = ride.optionalCount("viewPending");
        record.roomResidentMs = ride.optionalNumber("roomResidentMs");
        record.outcome = ride.string("outcome");
        if (auto walker = ride.optionalObject("walker")) {
            record.walker = readSpawn(*walker);
        }
        record.arrayBytes = ride.count("arrayBytes");
        record.gpuBytes = ride.count("gpuBytes");
        if (auto kinds = ride.optionalObject("gpuKinds")) {
            for (const gl::GpuMemoryKind kind : gl::kGpuMemoryKinds) {
                record.kindBytes[static_cast<std::size_t>(kind)] =
                    kinds->optionalCount(std::string(gl::toString(kind))).value_or(0);
            }
        }
        world.rides.push_back(std::move(record));
    }
    for (FieldReader& teleport : fields.optionalObjectArray("teleports")) {
        TeleportRecord record;
        record.frame = teleport.count("frame");
        record.to = teleport.string("to");
        record.reason = teleport.string("reason");
        if (auto walker = teleport.object("walker")) {
            record.walker = readSpawn(*walker);
        }
        world.teleports.push_back(std::move(record));
    }
    world.elevatorPromptFrames = fields.optionalCount("elevatorPromptFrames").value_or(0);
    world.totemPromptFrames = fields.optionalCount("totemPromptFrames").value_or(0);
    if (std::optional<FieldReader> showcase = fields.optionalObject("showcase")) {
        world.showcase.bound = true;
        world.showcase.games = showcase->optionalStringArray("games");
        world.showcase.builds = showcase->count("builds");
        world.showcase.unchanged = showcase->optionalCount("unchanged").value_or(0);
        for (FieldReader& draw : showcase->optionalObjectArray("draws")) {
            world.showcase.draws.push_back(
                ShowcaseDrawRecord{draw.count("meshes"), draw.count("materials"), draw.boolean("inPlace")});
        }
        world.showcase.cardsOpened = showcase->count("cardsOpened");
        world.showcase.lastCard = showcase->string("lastCard");
        world.showcase.hovered = showcase->string("hovered");
        world.showcase.frontsResident = showcase->count("frontsResident");
    }
    world.edits = readEdits(fields);
    world.regenerations = readRegenerations(fields); // I4b step 6 (absent from earlier reports)
    world.regenerationInputsReleased = fields.optionalCount("regenerationInputsReleased").value_or(0);
    // I5 step 4's review (absent from earlier reports).
    world.cabinetClipFrames = fields.optionalCount("cabinetClipFrames").value_or(0);
    world.cabinetCoverBeneathFrames = fields.optionalCount("cabinetCoverBeneathFrames").value_or(0);
    return world;
}

/// An unbound record (no room) is the empty object it was before I3 step 10b.
Json residencyToJson(const ResidencyRecord& residency) {
    Json json = Json::object();
    if (!residency.bound) {
        return json;
    }
    json["bound"] = true;
    json["format"] = residency.format;
    json["arrayBytes"] = residency.arrayBytes;
    json["items"] = residency.items;
    json["frontsResident"] = residency.frontsResident;
    json["spinesResident"] = residency.spinesResident;
    json["pending"] = residency.pending;
    json["fronts"] = Json{{"covers", residency.covers},
                          {"cachedCovers", residency.cachedCovers},
                          {"labels", residency.labels},
                          {"coverFailures", residency.coverFailures},
                          {"failed", residency.failed}};
    json["cache"] = Json{{"hits", residency.cacheHits},
                         {"hitsWithoutSource", residency.cacheHitsWithoutSource},
                         {"misses", residency.cacheMisses},
                         {"stale", residency.cacheStale},
                         {"corrupt", residency.cacheCorrupt},
                         {"stored", residency.cacheStored},
                         {"storeFailures", residency.cacheStoreFailures}};
    Json times = Json::object();
    putOptional(times, "shownAtMs", residency.shownAtMs);
    putOptional(times, "allResidentAtMs", residency.allResidentAtMs);
    putOptional(times, "residentAfterShownMs", residency.residentAfterShownMs);
    json["times"] = std::move(times);
    if (residency.allResidentFrame) {
        json["allResidentFrame"] = *residency.allResidentFrame;
    }
    json["uploadRounds"] = residency.uploadRounds;
    json["maxLayersPerRound"] = residency.maxLayersPerRound;
    Json arrival = Json{{"viewItems", residency.viewItems},
                        {"residentAtViewResident", residency.residentAtViewResident},
                        {"inViewAmongFirst", residency.inViewAmongFirst},
                        {"distanceQuarters", Json{{"q1", residency.arrivalDistanceQuarters[0]},
                                                  {"q2", residency.arrivalDistanceQuarters[1]},
                                                  {"q3", residency.arrivalDistanceQuarters[2]},
                                                  {"q4", residency.arrivalDistanceQuarters[3]}}}};
    putOptional(arrival, "viewResidentAfterShownMs", residency.viewResidentAfterShownMs);
    json["arrival"] = std::move(arrival);
    json["uploads"] = Json{{"overBudgetRounds", residency.overBudgetRounds}, {"maxRoundMs", residency.maxRoundMs}};
    json["lockstep"] = Json{{"waits", residency.lockstepWaits}, {"waitMs", residency.lockstepWaitMs}};
    json["pausedRounds"] = residency.pausedRounds;
    json["retries"] = Json{
        {"rounds", residency.retryRounds}, {"items", residency.retriedItems}, {"changed", residency.retriedChanged}};
    json["adopted"] = Json{{"items", residency.adoptedItems},
                           {"changed", residency.adoptedChanged},
                           {"resident", residency.adoptedResident}};
    const HeroRecord& hero = residency.hero;
    json["hero"] =
        Json{{"requests", hero.requests},   {"loaded", hero.loaded},           {"failed", hero.failed},
             {"evicted", hero.evicted},     {"resident", hero.resident},       {"bytes", hero.bytes},
             {"peakBytes", hero.peakBytes}, {"maxUploadMs", hero.maxUploadMs}, {"shownGame", hero.shownGame}};
    Json rooms = Json::array();
    for (const RoomResidencyRecord& room : residency.rooms) {
        rooms.push_back(Json{{"room", room.room},
                             {"items", room.items},
                             {"viewItems", room.viewItems},
                             {"frontsResident", room.frontsResident},
                             {"spinesResident", room.spinesResident},
                             {"pending", room.pending},
                             {"arrayBytes", room.arrayBytes},
                             {"arrays", room.arrays},
                             {"ownArrays", room.ownArrays},
                             {"uploads", room.uploads},
                             {"reuploads", room.reuploads}});
    }
    json["rooms"] = std::move(rooms);
    return json;
}

ResidencyRecord readResidency(FieldReader& fields) {
    ResidencyRecord residency;
    if (!fields.has("bound")) {
        return residency;
    }
    residency.bound = fields.boolean("bound");
    residency.format = fields.string("format");
    residency.arrayBytes = fields.count("arrayBytes");
    residency.items = fields.count("items");
    residency.frontsResident = fields.count("frontsResident");
    residency.spinesResident = fields.count("spinesResident");
    residency.pending = fields.count("pending");
    if (auto fronts = fields.object("fronts")) {
        residency.covers = fronts->count("covers");
        residency.cachedCovers = fronts->count("cachedCovers");
        residency.labels = fronts->count("labels");
        residency.coverFailures = fronts->count("coverFailures");
        residency.failed = fronts->count("failed");
    }
    if (auto cache = fields.object("cache")) {
        residency.cacheHits = cache->count("hits");
        residency.cacheHitsWithoutSource = cache->count("hitsWithoutSource");
        residency.cacheMisses = cache->count("misses");
        residency.cacheStale = cache->count("stale");
        residency.cacheCorrupt = cache->count("corrupt");
        residency.cacheStored = cache->count("stored");
        residency.cacheStoreFailures = cache->count("storeFailures");
    }
    if (auto times = fields.object("times")) {
        residency.shownAtMs = times->optionalNumber("shownAtMs");
        residency.allResidentAtMs = times->optionalNumber("allResidentAtMs");
        residency.residentAfterShownMs = times->optionalNumber("residentAfterShownMs");
    }
    residency.allResidentFrame = fields.optionalCount("allResidentFrame");
    residency.uploadRounds = fields.count("uploadRounds");
    residency.maxLayersPerRound = fields.count("maxLayersPerRound");
    if (auto arrival = fields.object("arrival")) {
        residency.viewItems = arrival->count("viewItems");
        residency.residentAtViewResident = arrival->count("residentAtViewResident");
        residency.inViewAmongFirst = arrival->count("inViewAmongFirst");
        residency.viewResidentAfterShownMs = arrival->optionalNumber("viewResidentAfterShownMs");
        if (auto quarters = arrival->object("distanceQuarters")) {
            residency.arrivalDistanceQuarters = {quarters->number("q1"), quarters->number("q2"), quarters->number("q3"),
                                                 quarters->number("q4")};
        }
    }
    if (auto uploads = fields.object("uploads")) {
        residency.overBudgetRounds = uploads->count("overBudgetRounds");
        residency.maxRoundMs = uploads->number("maxRoundMs");
    }
    if (auto lockstep = fields.object("lockstep")) {
        residency.lockstepWaits = lockstep->count("waits");
        residency.lockstepWaitMs = lockstep->number("waitMs");
    }
    residency.pausedRounds = fields.count("pausedRounds");
    if (auto retries = fields.object("retries")) {
        residency.retryRounds = retries->count("rounds");
        residency.retriedItems = retries->count("items");
        residency.retriedChanged = retries->count("changed");
    }
    if (auto adopted = fields.object("adopted")) {
        residency.adoptedItems = adopted->count("items");
        residency.adoptedChanged = adopted->count("changed");
        residency.adoptedResident = adopted->optionalCount("resident").value_or(0);
    }
    if (auto hero = fields.object("hero")) {
        HeroRecord& record = residency.hero;
        record.requests = hero->count("requests");
        record.loaded = hero->count("loaded");
        record.failed = hero->count("failed");
        record.evicted = hero->count("evicted");
        record.resident = hero->count("resident");
        record.bytes = hero->count("bytes");
        record.peakBytes = hero->count("peakBytes");
        record.maxUploadMs = hero->number("maxUploadMs");
        record.shownGame = hero->string("shownGame");
    }
    for (FieldReader& room : fields.optionalObjectArray("rooms")) {
        RoomResidencyRecord record;
        record.room = room.string("room");
        record.items = room.count("items");
        record.viewItems = room.count("viewItems");
        record.frontsResident = room.count("frontsResident");
        record.spinesResident = room.count("spinesResident");
        record.pending = room.count("pending");
        record.arrayBytes = room.count("arrayBytes");
        record.arrays = room.count("arrays");
        record.ownArrays = room.count("ownArrays");
        record.uploads = room.optionalCount("uploads").value_or(0);
        record.reuploads = room.optionalCount("reuploads").value_or(0);
        residency.rooms.push_back(std::move(record));
    }
    return residency;
}

/// An unbound record (no hero CRT) is the empty object it was before I3 step 11.
Json videoToJson(const VideoRecord& video) {
    Json json = Json::object();
    if (!video.bound) {
        return json;
    }
    json["bound"] = true;
    json["screen"] = video.screen;
    json["gameId"] = video.gameId;
    json["picture"] = video.picture;
    json["phase"] = video.phase;
    json["decoderState"] = video.decoderState;
    putOptional(json, "freezePtsSeconds", video.freezePtsSeconds);
    json["frozenFrameShown"] = video.frozenFrameShown;
    json["selections"] = video.selections;
    json["targets"] = video.targets;
    json["decodersStarted"] = video.decodersStarted;
    json["maxDecoderThreads"] = video.maxDecoderThreads;
    json["framesUploaded"] = video.framesUploaded;
    json["ringBusy"] = video.ringBusy;
    json["pauses"] = video.pauses;
    putOptional(json, "firstFrameMs", video.firstFrameMs);
    json["clipFramesUploaded"] = video.clipFramesUploaded;
    putOptional(json, "firstPtsSeconds", video.firstPtsSeconds);
    putOptional(json, "lastPtsSeconds", video.lastPtsSeconds);
    json["clipUploadSeconds"] = video.clipUploadSeconds;
    json["videoBytes"] = video.videoBytes;
    json["stillBytes"] = video.stillBytes;
    json["joinedAtExit"] = video.joinedAtExit;
    json["waitsBeforeExit"] = video.waitsBeforeExit;
    Json fallbacks = Json::array();
    for (const VideoFallbackRecord& fallback : video.fallbacks) {
        fallbacks.push_back(
            Json{{"gameId", fallback.gameId}, {"picture", fallback.picture}, {"reason", fallback.reason}});
    }
    json["fallbacks"] = std::move(fallbacks);
    Json launches = Json::array();
    for (const VideoLaunchRecord& launch : video.launches) {
        Json entry;
        entry["decoderRan"] = launch.decoderRan;
        entry["stopMs"] = launch.stopMs;
        entry["threadsRunning"] = launch.threadsRunning;
        if (launch.stopFrame) {
            entry["stopFrame"] = *launch.stopFrame;
        }
        entry["waitMs"] = launch.waitMs;
        if (launch.handOverFrame) {
            entry["handOverFrame"] = *launch.handOverFrame;
        }
        entry["cancelled"] = launch.cancelled;
        putOptional(entry, "positionSeconds", launch.positionSeconds);
        entry["reopened"] = launch.reopened;
        putOptional(entry, "reopenedAtSeconds", launch.reopenedAtSeconds);
        launches.push_back(std::move(entry));
    }
    json["launches"] = std::move(launches);
    Json binds = Json::array();
    for (const VideoBindRecord& bind : video.binds) {
        binds.push_back(Json{{"frame", bind.frame},
                             {"screen", bind.screen},
                             {"games", bind.games},
                             {"bindMs", bind.bindMs},
                             {"threadsRunning", bind.threadsRunning},
                             {"clipStopped", bind.clipStopped}});
    }
    json["binds"] = std::move(binds);
    json["rebinds"] = video.rebinds;
    json["playlistMode"] = video.playlistMode;
    json["playlistRoom"] = video.playlistRoom;
    json["previewSeconds"] = video.previewSeconds;
    json["playlistOrder"] = video.playlistOrder;
    json["origin"] = video.origin;
    Json played = Json::array();
    for (const VideoPlayedRecord& entry : video.played) {
        played.push_back(Json{{"atSeconds", entry.atSeconds}, {"gameId", entry.gameId}, {"origin", entry.origin}});
    }
    json["played"] = std::move(played);
    Json shown = Json::array();
    for (const VideoShownRecord& entry : video.shown) {
        shown.push_back(Json{{"atSeconds", entry.atSeconds}, {"gameId", entry.gameId}, {"picture", entry.picture}});
    }
    json["shown"] = std::move(shown);
    json["heldFrames"] = video.heldFrames;
    json["idleFramesWithGame"] = video.idleFramesWithGame;
    json["maxDecoders"] = video.maxDecoders;
    json["maxOpenDecoders"] = video.maxOpenDecoders;
    json["handOvers"] = video.handOvers;
    json["maxUploadBytes"] = video.maxUploadBytes;
    json["deferredUploads"] = video.deferredUploads;
    json["oversizeUploads"] = video.oversizeUploads;
    Json screens = Json::array();
    for (const VideoScreenRecord& screen : video.screens) {
        Json entry{{"screen", screen.screen},
                   {"kind", screen.kind},
                   {"screenClass", screen.screenClass},
                   {"gameId", screen.gameId},
                   {"picture", screen.picture},
                   {"playing", screen.playing},
                   {"textureId", screen.textureId},
                   {"visible", screen.visible},
                   {"pictureRect", screen.pictureRect},
                   {"heldFrame", screen.heldFrame},
                   {"bounds", screen.bounds}};
        if (screen.slot) {
            entry["slot"] = *screen.slot;
        }
        screens.push_back(std::move(entry));
    }
    json["screens"] = std::move(screens);
    Json cabinetEvents = Json::array();
    for (const VideoCabinetEventRecord& event : video.cabinetEvents) {
        cabinetEvents.push_back(Json{{"frame", event.frame},
                                     {"atSeconds", event.atSeconds},
                                     {"screen", event.screen},
                                     {"gameId", event.gameId},
                                     {"screenClass", event.screenClass},
                                     {"picture", event.picture}});
    }
    json["cabinetEvents"] = std::move(cabinetEvents);
    json["cabinetEventsDropped"] = video.cabinetEventsDropped;
    json["heldFrameShows"] = video.heldFrameShows;
    json["firstFrameWaits"] = video.firstFrameWaits;
    json["firstFrameWaitMaxMs"] = video.firstFrameWaitMaxMs;
    json["firstFrameWaitTimeouts"] = video.firstFrameWaitTimeouts;
    Json events = Json::array();
    for (const VideoSlotEventRecord& event : video.slotEvents) {
        Json entry{{"frame", event.frame},
                   {"atSeconds", event.atSeconds},
                   {"slot", event.slot},
                   {"action", event.action},
                   {"gameId", event.gameId},
                   {"reason", event.reason},
                   {"screens", event.screens}};
        if (event.fromSeconds) {
            entry["fromSeconds"] = *event.fromSeconds;
        }
        events.push_back(std::move(entry));
    }
    json["slotEvents"] = std::move(events);
    Json drops = Json::array();
    for (const VideoBudgetDropRecord& drop : video.budgetDrops) {
        drops.push_back(Json{{"atSeconds", drop.atSeconds},
                             {"slot", drop.slot},
                             {"gameId", drop.gameId},
                             {"ratio", drop.ratio},
                             {"untilSeconds", drop.untilSeconds}});
    }
    json["budgetDrops"] = std::move(drops);
    json["slotEventsDropped"] = video.slotEventsDropped;
    const auto verdictList = [](const std::vector<VideoVerdictRecord>& records) {
        Json list = Json::array();
        for (const VideoVerdictRecord& record : records) {
            list.push_back(Json{{"gameId", record.gameId}, {"verdict", record.verdict}});
        }
        return list;
    };
    json["verdicts"] = Json{{"loaded", video.verdicts.loaded},
                            {"entries", video.verdicts.entries},
                            {"knownBad", verdictList(video.verdicts.knownBad)},
                            {"given", verdictList(video.verdicts.given)},
                            {"written", video.verdicts.written},
                            {"writeFailures", video.verdicts.writeFailures}};
    const VideoViewerRecord& viewer = video.viewer;
    json["viewer"] = Json{{"clips", viewer.clips},
                          {"decodersOpened", viewer.decodersOpened},
                          {"framesUploaded", viewer.framesUploaded},
                          {"pauses", viewer.pauses},
                          {"knownBad", viewer.knownBad},
                          {"failures", viewer.failures},
                          {"game", viewer.game},
                          {"state", viewer.state},
                          {"reason", viewer.reason},
                          {"width", viewer.width},
                          {"height", viewer.height},
                          {"rows", viewer.rows},
                          {"maxGpuBytes", viewer.maxGpuBytes},
                          {"gpuBytes", viewer.gpuBytes},
                          {"openFrames", viewer.openFrames},
                          {"worldFramesWhileOpen", viewer.worldFramesWhileOpen}};
    return json;
}

Json processPriorityToJson(const ProcessPriorityRecord& priority) {
    Json changes = Json::array();
    for (const PriorityChangeRecord& change : priority.changes) {
        changes.push_back(Json{{"frame", change.frame},
                               {"priority", change.priority},
                               {"reason", change.reason},
                               {"applied", change.applied}});
    }
    return Json{{"changes", std::move(changes)}};
}

ProcessPriorityRecord readProcessPriority(FieldReader& fields) {
    ProcessPriorityRecord priority;
    for (FieldReader& change : fields.objectArray("changes")) {
        priority.changes.push_back(PriorityChangeRecord{change.count("frame"), change.string("priority"),
                                                        change.string("reason"), change.boolean("applied")});
    }
    return priority;
}

Json crtLooksToJson(const CrtLooksRecord& looks) {
    Json screens = Json::array();
    for (const CrtLookRecord& screen : looks.screens) {
        screens.push_back(Json{{"screen", screen.screen},
                               {"room", screen.room},
                               {"kind", screen.kind},
                               {"preset", screen.preset},
                               {"chosen", screen.chosen},
                               {"rule", screen.rule}});
    }
    return Json{{"crtEffect", looks.crtEffect}, {"presetsSource", looks.presetsSource}, {"screens", std::move(screens)}};
}

CrtLooksRecord readCrtLooks(FieldReader& fields) {
    CrtLooksRecord looks;
    looks.crtEffect = fields.string("crtEffect");
    looks.presetsSource = fields.string("presetsSource");
    for (FieldReader& screen : fields.objectArray("screens")) {
        looks.screens.push_back(CrtLookRecord{screen.string("screen"), screen.string("room"), screen.string("kind"),
                                              screen.string("preset"), screen.string("chosen"),
                                              screen.string("rule")});
    }
    return looks;
}

Json audioToJson(const AudioRecord& audio) {
    Json json;
    json["device"] = audio.device;
    json["reason"] = audio.reason;
    json["deviceName"] = audio.deviceName;
    json["sampleRate"] = audio.sampleRate;
    json["mutedAtStart"] = audio.mutedAtStart;
    json["muted"] = audio.muted;
    json["muteToggles"] = audio.muteToggles;
    json["duckedFrames"] = audio.duckedFrames;
    json["duckChanges"] = audio.duckChanges;
    json["blocks"] = audio.blocks;
    json["framesMixed"] = audio.framesMixed;
    if (audio.capture) {
        json["capture"] = core::pathToGenericUtf8(*audio.capture);
    }
    if (audio.captureStartSeconds) {
        json["captureStartSeconds"] = *audio.captureStartSeconds;
    }
    json["sync"] = Json{{"blocks", audio.syncBlocks},
                        {"resyncs", audio.resyncs},
                        {"corrections", audio.corrections},
                        {"underrunFrames", audio.underrunFrames},
                        {"overflowFrames", audio.overflowFrames},
                        {"p50Milliseconds", audio.syncP50Milliseconds},
                        {"p95Milliseconds", audio.syncP95Milliseconds},
                        {"maxMilliseconds", audio.syncMaxMilliseconds}};
    json["suspends"] = audio.suspends;
    json["deviceStops"] = audio.deviceStops;
    json["deviceStarts"] = audio.deviceStarts;
    json["deviceFailures"] = audio.deviceFailures;
    json["deviceLosses"] = audio.deviceLosses;
    json["deviceRecoveries"] = audio.deviceRecoveries;
    Json audible = Json::array();
    for (const AudioAudibleRecord& change : audio.audible) {
        audible.push_back(Json{{"frame", change.frame},
                               {"atSeconds", change.atSeconds},
                               {"screen", change.screen},
                               {"gameId", change.gameId}});
    }
    json["audible"] = std::move(audible);
    Json levels = Json::array();
    for (const AudioLevelRecord& level : audio.levels) {
        levels.push_back(Json{{"atSeconds", level.atSeconds},
                              {"screen", level.screen},
                              {"distance", level.distance},
                              {"spatialGain", level.spatialGain},
                              {"pan", level.pan},
                              {"gain", level.gain}});
    }
    json["levels"] = std::move(levels);
    Json presented = Json::array();
    for (const AudioPresentedRecord& frame : audio.presented) {
        presented.push_back(Json{{"atSeconds", frame.atSeconds}, {"ptsSeconds", frame.ptsSeconds}});
    }
    json["presented"] = std::move(presented);
    Json clips = Json::array();
    for (const AudioClipRecord& clip : audio.clips) {
        clips.push_back(Json{{"gameId", clip.gameId},
                             {"codec", clip.codec},
                             {"sampleRate", clip.sampleRate},
                             {"channels", clip.channels}});
    }
    json["clips"] = std::move(clips);
    return json;
}

AudioRecord readAudio(FieldReader& fields) {
    AudioRecord audio;
    audio.device = fields.string("device");
    audio.reason = fields.string("reason");
    audio.deviceName = fields.string("deviceName");
    audio.sampleRate = fields.count("sampleRate");
    audio.mutedAtStart = fields.boolean("mutedAtStart");
    audio.muted = fields.boolean("muted");
    audio.muteToggles = fields.count("muteToggles");
    audio.duckedFrames = fields.count("duckedFrames");
    audio.duckChanges = fields.count("duckChanges");
    audio.blocks = fields.count("blocks");
    audio.framesMixed = fields.count("framesMixed");
    if (const std::optional<std::string> capture = fields.optionalString("capture")) {
        audio.capture = core::pathFromUtf8(*capture);
    }
    audio.captureStartSeconds = fields.optionalNumber("captureStartSeconds");
    if (auto sync = fields.object("sync")) {
        audio.syncBlocks = sync->count("blocks");
        audio.resyncs = sync->count("resyncs");
        audio.corrections = sync->count("corrections");
        audio.underrunFrames = sync->count("underrunFrames");
        audio.overflowFrames = sync->count("overflowFrames");
        audio.syncP50Milliseconds = sync->number("p50Milliseconds");
        audio.syncP95Milliseconds = sync->number("p95Milliseconds");
        audio.syncMaxMilliseconds = sync->number("maxMilliseconds");
    }
    audio.suspends = fields.count("suspends");
    audio.deviceStops = fields.count("deviceStops");
    audio.deviceStarts = fields.count("deviceStarts");
    audio.deviceFailures = fields.count("deviceFailures");
    audio.deviceLosses = fields.count("deviceLosses");
    audio.deviceRecoveries = fields.count("deviceRecoveries");
    for (FieldReader& change : fields.objectArray("audible")) {
        audio.audible.push_back(AudioAudibleRecord{change.count("frame"), change.number("atSeconds"),
                                                   change.string("screen"), change.string("gameId")});
    }
    for (FieldReader& level : fields.objectArray("levels")) {
        audio.levels.push_back(AudioLevelRecord{level.number("atSeconds"), level.string("screen"),
                                                level.number("distance"), level.number("spatialGain"),
                                                level.number("pan"), level.number("gain")});
    }
    for (FieldReader& frame : fields.objectArray("presented")) {
        audio.presented.push_back(AudioPresentedRecord{frame.number("atSeconds"), frame.number("ptsSeconds")});
    }
    for (FieldReader& clip : fields.objectArray("clips")) {
        audio.clips.push_back(AudioClipRecord{clip.string("gameId"), clip.string("codec"), clip.count("sampleRate"),
                                              clip.count("channels")});
    }
    return audio;
}

/// What videoToJson() wrote: the empty object, or every field of a bound record.
VideoRecord readVideo(FieldReader& fields) {
    VideoRecord video;
    if (!fields.has("bound")) {
        return video;
    }
    video.bound = fields.boolean("bound");
    video.screen = fields.string("screen");
    video.gameId = fields.string("gameId");
    video.picture = fields.string("picture");
    video.phase = fields.string("phase");
    video.decoderState = fields.string("decoderState");
    video.freezePtsSeconds = fields.optionalNumber("freezePtsSeconds");
    video.frozenFrameShown = fields.boolean("frozenFrameShown");
    video.selections = fields.count("selections");
    video.targets = fields.count("targets");
    video.decodersStarted = fields.count("decodersStarted");
    video.maxDecoderThreads = fields.count("maxDecoderThreads");
    video.framesUploaded = fields.count("framesUploaded");
    video.ringBusy = fields.count("ringBusy");
    video.pauses = fields.count("pauses");
    video.firstFrameMs = fields.optionalNumber("firstFrameMs");
    video.clipFramesUploaded = fields.count("clipFramesUploaded");
    video.firstPtsSeconds = fields.optionalNumber("firstPtsSeconds");
    video.lastPtsSeconds = fields.optionalNumber("lastPtsSeconds");
    video.clipUploadSeconds = fields.number("clipUploadSeconds");
    video.videoBytes = fields.count("videoBytes");
    video.stillBytes = fields.count("stillBytes");
    video.joinedAtExit = fields.boolean("joinedAtExit");
    video.waitsBeforeExit = fields.optionalCount("waitsBeforeExit").value_or(0);
    for (FieldReader& fallback : fields.objectArray("fallbacks")) {
        video.fallbacks.push_back(
            VideoFallbackRecord{fallback.string("gameId"), fallback.string("picture"), fallback.string("reason")});
    }
    for (FieldReader& launch : fields.objectArray("launches")) {
        VideoLaunchRecord entry;
        entry.decoderRan = launch.boolean("decoderRan");
        entry.stopMs = launch.number("stopMs");
        entry.threadsRunning = launch.count("threadsRunning");
        entry.stopFrame = launch.optionalCount("stopFrame");
        entry.waitMs = launch.optionalNumber("waitMs").value_or(0.0);
        entry.handOverFrame = launch.optionalCount("handOverFrame");
        entry.cancelled = launch.has("cancelled") && launch.boolean("cancelled");
        entry.positionSeconds = launch.optionalNumber("positionSeconds");
        entry.reopened = launch.boolean("reopened");
        entry.reopenedAtSeconds = launch.optionalNumber("reopenedAtSeconds");
        video.launches.push_back(entry);
    }
    for (FieldReader& bind : fields.optionalObjectArray("binds")) {
        VideoBindRecord entry;
        entry.frame = bind.count("frame");
        entry.screen = bind.string("screen");
        entry.games = bind.count("games");
        entry.bindMs = bind.number("bindMs");
        entry.threadsRunning = bind.count("threadsRunning");
        entry.clipStopped = bind.has("clipStopped") && bind.boolean("clipStopped");
        video.binds.push_back(std::move(entry));
    }
    video.rebinds = fields.optionalCount("rebinds").value_or(0); // I5 step 3's review
    // I5 step 1's playlist (absent from earlier reports).
    if (fields.has("playlistMode")) {
        video.playlistMode = fields.string("playlistMode");
        video.playlistRoom = fields.string("playlistRoom");
        video.previewSeconds = fields.number("previewSeconds");
        video.playlistOrder = fields.stringArray("playlistOrder");
        video.origin = fields.string("origin");
    }
    for (FieldReader& entry : fields.optionalObjectArray("played")) {
        video.played.push_back(
            VideoPlayedRecord{entry.number("atSeconds"), entry.string("gameId"), entry.string("origin")});
    }
    for (FieldReader& entry : fields.optionalObjectArray("shown")) {
        video.shown.push_back(
            VideoShownRecord{entry.number("atSeconds"), entry.string("gameId"), entry.string("picture")});
    }
    video.heldFrames = fields.optionalCount("heldFrames").value_or(0);
    video.idleFramesWithGame = fields.optionalCount("idleFramesWithGame").value_or(0);
    // I5 step 2's scheduler (absent from earlier reports).
    video.maxDecoders = fields.optionalCount("maxDecoders").value_or(0);
    video.maxOpenDecoders = fields.optionalCount("maxOpenDecoders").value_or(0);
    video.handOvers = fields.optionalCount("handOvers").value_or(0);
    video.maxUploadBytes = fields.optionalCount("maxUploadBytes").value_or(0);
    video.deferredUploads = fields.optionalCount("deferredUploads").value_or(0);
    video.oversizeUploads = fields.optionalCount("oversizeUploads").value_or(0);
    for (FieldReader& entry : fields.optionalObjectArray("screens")) {
        VideoScreenRecord screen;
        screen.screen = entry.string("screen");
        screen.kind = entry.string("kind");
        screen.screenClass = entry.string("screenClass");
        screen.gameId = entry.string("gameId");
        screen.picture = entry.string("picture");
        screen.slot = entry.optionalCount("slot");
        screen.playing = entry.boolean("playing");
        screen.textureId = entry.count("textureId");
        screen.visible = entry.boolean("visible");
        // I5 step 4's (absent from earlier reports).
        if (entry.has("pictureRect")) {
            screen.pictureRect = entry.numberArray("pictureRect");
        }
        screen.heldFrame = entry.has("heldFrame") && entry.boolean("heldFrame");
        // I5 step 3's review (absent from earlier reports).
        if (entry.has("bounds")) {
            screen.bounds = entry.numberArray("bounds");
        }
        video.screens.push_back(std::move(screen));
    }
    for (FieldReader& entry : fields.optionalObjectArray("cabinetEvents")) {
        VideoCabinetEventRecord event;
        event.frame = entry.count("frame");
        event.atSeconds = entry.number("atSeconds");
        event.screen = entry.string("screen");
        event.gameId = entry.string("gameId");
        event.screenClass = entry.string("screenClass");
        event.picture = entry.string("picture");
        video.cabinetEvents.push_back(std::move(event));
    }
    video.cabinetEventsDropped = fields.optionalCount("cabinetEventsDropped").value_or(0);
    video.heldFrameShows = fields.optionalCount("heldFrameShows").value_or(0);
    // I5 step 4's review (absent from earlier reports).
    video.firstFrameWaits = fields.optionalCount("firstFrameWaits").value_or(0);
    video.firstFrameWaitMaxMs = fields.optionalNumber("firstFrameWaitMaxMs").value_or(0.0);
    video.firstFrameWaitTimeouts = fields.optionalCount("firstFrameWaitTimeouts").value_or(0);
    for (FieldReader& entry : fields.optionalObjectArray("slotEvents")) {
        VideoSlotEventRecord event;
        event.frame = entry.count("frame");
        event.atSeconds = entry.number("atSeconds");
        event.slot = entry.count("slot");
        event.action = entry.string("action");
        event.gameId = entry.string("gameId");
        event.reason = entry.string("reason");
        event.screens = entry.stringArray("screens");
        event.fromSeconds = entry.optionalNumber("fromSeconds");
        video.slotEvents.push_back(std::move(event));
    }
    for (FieldReader& entry : fields.optionalObjectArray("budgetDrops")) {
        video.budgetDrops.push_back(VideoBudgetDropRecord{entry.number("atSeconds"), entry.count("slot"),
                                                          entry.string("gameId"), entry.number("ratio"),
                                                          entry.number("untilSeconds")});
    }
    video.slotEventsDropped = fields.optionalCount("slotEventsDropped").value_or(0);
    if (std::optional<FieldReader> verdicts = fields.optionalObject("verdicts")) {
        video.verdicts.loaded = verdicts->boolean("loaded");
        video.verdicts.entries = verdicts->count("entries");
        for (FieldReader& entry : verdicts->objectArray("knownBad")) {
            video.verdicts.knownBad.push_back(VideoVerdictRecord{entry.string("gameId"), entry.string("verdict")});
        }
        for (FieldReader& entry : verdicts->objectArray("given")) {
            video.verdicts.given.push_back(VideoVerdictRecord{entry.string("gameId"), entry.string("verdict")});
        }
        video.verdicts.written = verdicts->count("written");
        video.verdicts.writeFailures = verdicts->count("writeFailures");
    }
    // I5 step 8 (absent from earlier reports).
    if (std::optional<FieldReader> viewer = fields.optionalObject("viewer")) {
        VideoViewerRecord& record = video.viewer;
        record.clips = viewer->count("clips");
        record.decodersOpened = viewer->count("decodersOpened");
        record.framesUploaded = viewer->count("framesUploaded");
        record.pauses = viewer->count("pauses");
        record.knownBad = viewer->count("knownBad");
        record.failures = viewer->count("failures");
        record.game = viewer->string("game");
        record.state = viewer->string("state");
        record.reason = viewer->string("reason");
        record.width = viewer->count("width");
        record.height = viewer->count("height");
        record.rows = viewer->count("rows");
        record.maxGpuBytes = viewer->count("maxGpuBytes");
        record.gpuBytes = viewer->optionalCount("gpuBytes").value_or(0); // I5 step 8's review
        record.openFrames = viewer->count("openFrames");
        record.worldFramesWhileOpen = viewer->count("worldFramesWhileOpen");
    }
    return video;
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

Json refreshToJson(const RefreshRecord& refresh) {
    Json json;
    json["trigger"] = refresh.trigger;
    json["outcome"] = refresh.outcome;
    json["error"] = refresh.error;
    Json frames;
    frames["request"] = refresh.requestFrame;
    if (refresh.endFrame) {
        frames["end"] = *refresh.endFrame;
    }
    json["frames"] = std::move(frames);
    Json times = Json::object();
    putOptional(times, "ms", refresh.ms);
    putOptional(times, "longestSliceMs", refresh.longestSliceMs);
    putOptional(times, "stopMs", refresh.stopMs);
    json["times"] = std::move(times);
    json["entries"] = refresh.entries;
    json["added"] = refresh.added;
    json["removed"] = refresh.removed;
    json["changed"] = refresh.changed;
    json["moved"] = refresh.moved;
    json["reviews"] = refresh.reviews;
    json["guarded"] = refresh.guarded;
    json["slices"] = refresh.slices;
    json["served"] = refresh.served;
    if (refresh.lists) {
        json["lists"] = timesToJson(*refresh.lists);
    }
    if (refresh.before) {
        json["before"] = viewToJson(*refresh.before);
    }
    if (refresh.after) {
        json["after"] = viewToJson(*refresh.after);
    }
    return json;
}

RefreshRecord readRefresh(FieldReader& fields) {
    RefreshRecord refresh;
    refresh.trigger = fields.string("trigger");
    refresh.outcome = fields.string("outcome");
    refresh.error = fields.string("error");
    if (auto frames = fields.object("frames")) {
        refresh.requestFrame = frames->count("request");
        refresh.endFrame = frames->optionalCount("end");
    }
    if (auto times = fields.object("times")) {
        refresh.ms = times->optionalNumber("ms");
        refresh.longestSliceMs = times->optionalNumber("longestSliceMs");
        refresh.stopMs = times->optionalNumber("stopMs");
    }
    refresh.entries = static_cast<std::size_t>(fields.count("entries", kMaxSize));
    refresh.added = static_cast<std::size_t>(fields.count("added", kMaxSize));
    refresh.removed = static_cast<std::size_t>(fields.count("removed", kMaxSize));
    refresh.changed = static_cast<std::size_t>(fields.count("changed", kMaxSize));
    refresh.moved = static_cast<std::size_t>(fields.count("moved", kMaxSize));
    refresh.reviews = static_cast<std::size_t>(fields.count("reviews", kMaxSize));
    refresh.guarded = static_cast<std::size_t>(fields.count("guarded", kMaxSize));
    refresh.slices = fields.count("slices");
    refresh.served = fields.count("served");
    if (auto lists = fields.optionalObject("lists")) {
        refresh.lists = readTimes(*lists);
    }
    if (auto before = fields.optionalObject("before")) {
        refresh.before = readView(*before);
    }
    if (auto after = fields.optionalObject("after")) {
        refresh.after = readView(*after);
    }
    return refresh;
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
    // I3 step 10b (optional when read).
    if (catalog.lists) {
        json["lists"] = timesToJson(*catalog.lists);
    }
    json["interruptedSessions"] = catalog.interruptedSessions;
    json["cardSqueezed"] = catalog.cardSqueezed;
    // I3 step 13r (optional when read).
    Json refreshes = Json::array();
    for (const RefreshRecord& refresh : catalog.refreshes) {
        refreshes.push_back(refreshToJson(refresh));
    }
    json["refreshes"] = std::move(refreshes);
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
    if (auto lists = fields.optionalObject("lists")) {
        catalog.lists = readTimes(*lists);
    }
    catalog.interruptedSessions =
        static_cast<std::size_t>(fields.optionalCount("interruptedSessions", kMaxSize).value_or(0));
    if (fields.has("cardSqueezed")) {
        catalog.cardSqueezed = fields.stringArray("cardSqueezed");
    }
    // Written since I3 step 13r: optional as well.
    for (FieldReader& refresh : fields.optionalObjectArray("refreshes")) {
        catalog.refreshes.push_back(readRefresh(refresh));
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

/// A size as the report's vectors are written: three numbers (FieldReader::vec3), the third 0.
Json ivec2ToJson(const glm::ivec2& value) {
    return Json::array({value.x, value.y, 0});
}

glm::ivec2 readIvec2(FieldReader& fields, std::string_view key) {
    const glm::vec3 padded = fields.vec3(key);
    return glm::ivec2(static_cast<int>(padded.x), static_cast<int>(padded.y));
}

Json xrToJson(const XrRecord& xr) {
    Json json;
    json["runtimeChoice"] = xr.runtimeChoice;
    json["started"] = xr.started;
    if (xr.unavailable) {
        json["unavailable"] = *xr.unavailable;
    }
    json["runtime"] = xr.runtime;
    json["runtimeVersion"] = xr.runtimeVersion;
    json["system"] = xr.system;
    json["extensions"] = xr.extensions;
    json["space"] = xr.space;
    json["recommendedEyeSize"] = ivec2ToJson(xr.recommendedEyeSize);
    json["eyeSize"] = ivec2ToJson(xr.eyeSize);
    json["renderScale"] = xr.renderScale;
    json["format"] = xr.format;
    json["srgb"] = xr.srgb;
    json["imagesPerEye"] = xr.imagesPerEye;
    Json rates = Json::array();
    for (const float rate : xr.refreshRates) {
        rates.push_back(static_cast<double>(rate));
    }
    json["refreshRates"] = std::move(rates);
    if (xr.refreshRate) {
        json["refreshRate"] = static_cast<double>(*xr.refreshRate);
    }
    json["reachedFocused"] = xr.reachedFocused;
    json["endReason"] = xr.endReason;
    if (xr.desktopFromFrame) {
        json["desktopFromFrame"] = *xr.desktopFromFrame;
    }
    json["framesWaited"] = xr.framesWaited;
    json["framesWithViews"] = xr.framesWithViews;
    json["refreshHz"] = xr.refreshHz;
    json["missedRefreshes"] = xr.missedRefreshes;
    json["missedFraction"] = xr.missedFraction;
    Json cpu;
    putTimes(cpu, xr.cpuTimes);
    json["cpuTimes"] = std::move(cpu);
    json["events"] = xr.events;
    Json head = Json::array();
    for (const XrHeadSample& sample : xr.head) {
        head.push_back(Json{{"frame", sample.frame},
                            {"position", vec3ToJson(sample.position)},
                            {"yawDeg", static_cast<double>(sample.yawDeg)},
                            {"pitchDeg", static_cast<double>(sample.pitchDeg)},
                            {"rollDeg", static_cast<double>(sample.rollDeg)}});
    }
    json["head"] = std::move(head);
    json["posture"] = xr.posture;
    json["turn"] = xr.turn;
    json["snapTurns"] = xr.snapTurns;
    json["teleports"] = xr.teleports;
    json["pushedFrames"] = xr.pushedFrames;
    json["largestHeadOffset"] = static_cast<double>(xr.largestHeadOffset);
    json["interactionProfiles"] = xr.interactionProfiles;
    json["leftController"] = xr.leftController;
    json["rightController"] = xr.rightController;
    json["handFrames"] = xr.handFrames;
    json["pointedFrames"] = xr.pointedFrames;
    json["vibrations"] = xr.vibrations;
    json["uiPanelFrames"] = xr.uiPanelFrames;
    json["uiPointedFrames"] = xr.uiPointedFrames;
    json["uiClicks"] = xr.uiClicks;
    json["captureStarts"] = xr.captureStarts;
    json["captureFrames"] = xr.captureFrames;
    json["captureShownFrames"] = xr.captureShownFrames;
    json["captureSize"] = ivec2ToJson(xr.captureSize);
    json["captureError"] = xr.captureError;
    json["captureCrop"] = Json::array({static_cast<double>(xr.captureCrop.x), static_cast<double>(xr.captureCrop.y),
                                       static_cast<double>(xr.captureCrop.z), static_cast<double>(xr.captureCrop.w)});
    json["captureCropSamples"] = xr.captureCropSamples;
    json["captureCropChanges"] = xr.captureCropChanges;
    json["playingWalkFrames"] = xr.playingWalkFrames;
    json["dynamicScale"] = xr.dynamicScale;
    json["quality"] = xr.quality;
    json["scaleChanges"] = xr.scaleChanges;
    json["lowestScale"] = static_cast<double>(xr.lowestScale);
    json["eyesGpuMilliseconds"] = xr.eyesGpuMilliseconds;
    return json;
}

XrRecord readXr(FieldReader& fields) {
    XrRecord xr;
    xr.runtimeChoice = fields.string("runtimeChoice");
    xr.started = fields.boolean("started");
    xr.unavailable = fields.optionalString("unavailable");
    xr.runtime = fields.string("runtime");
    xr.runtimeVersion = fields.string("runtimeVersion");
    xr.system = fields.string("system");
    xr.extensions = fields.stringArray("extensions");
    xr.space = fields.string("space");
    xr.recommendedEyeSize = readIvec2(fields, "recommendedEyeSize");
    xr.eyeSize = readIvec2(fields, "eyeSize");
    xr.renderScale = fields.number("renderScale");
    xr.format = fields.string("format");
    xr.srgb = fields.boolean("srgb");
    xr.imagesPerEye = static_cast<std::size_t>(fields.count("imagesPerEye", kMaxSize));
    for (const double rate : fields.numberArray("refreshRates")) {
        xr.refreshRates.push_back(static_cast<float>(rate));
    }
    if (const auto rate = fields.optionalNumber("refreshRate")) {
        xr.refreshRate = static_cast<float>(*rate);
    }
    xr.reachedFocused = fields.boolean("reachedFocused");
    xr.endReason = fields.string("endReason");
    xr.desktopFromFrame = fields.optionalCount("desktopFromFrame");
    xr.framesWaited = fields.count("framesWaited");
    xr.framesWithViews = fields.count("framesWithViews");
    xr.refreshHz = fields.number("refreshHz");
    xr.missedRefreshes = fields.count("missedRefreshes");
    xr.missedFraction = fields.number("missedFraction");
    if (auto cpu = fields.optionalObject("cpuTimes")) {
        xr.cpuTimes = readOptionalTimes(*cpu);
    }
    xr.events = fields.stringArray("events");
    for (FieldReader& sample : fields.objectArray("head")) {
        xr.head.push_back(XrHeadSample{sample.count("frame"), sample.vec3("position"),
                                       static_cast<float>(sample.number("yawDeg")),
                                       static_cast<float>(sample.number("pitchDeg")),
                                       static_cast<float>(sample.number("rollDeg"))});
    }
    xr.posture = fields.string("posture");
    xr.turn = fields.string("turn");
    xr.snapTurns = fields.count("snapTurns");
    xr.teleports = fields.count("teleports");
    xr.pushedFrames = fields.count("pushedFrames");
    xr.largestHeadOffset = static_cast<float>(fields.number("largestHeadOffset"));
    xr.interactionProfiles = fields.stringArray("interactionProfiles");
    xr.leftController = fields.string("leftController");
    xr.rightController = fields.string("rightController");
    xr.handFrames = fields.count("handFrames");
    xr.pointedFrames = fields.count("pointedFrames");
    xr.vibrations = fields.count("vibrations");
    xr.uiPanelFrames = fields.count("uiPanelFrames");
    xr.uiPointedFrames = fields.count("uiPointedFrames");
    xr.uiClicks = fields.count("uiClicks");
    xr.captureStarts = fields.count("captureStarts");
    xr.captureFrames = fields.count("captureFrames");
    xr.captureShownFrames = fields.count("captureShownFrames");
    xr.captureSize = readIvec2(fields, "captureSize");
    xr.captureError = fields.string("captureError");
    xr.captureCrop = fields.vec4("captureCrop");
    xr.captureCropSamples = fields.count("captureCropSamples");
    xr.captureCropChanges = fields.count("captureCropChanges");
    xr.playingWalkFrames = fields.count("playingWalkFrames");
    xr.dynamicScale = fields.boolean("dynamicScale");
    xr.quality = fields.string("quality");
    xr.scaleChanges = fields.count("scaleChanges");
    xr.lowestScale = static_cast<float>(fields.number("lowestScale"));
    xr.eyesGpuMilliseconds = fields.number("eyesGpuMilliseconds");
    return xr;
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
                                  {"height", report.screenshot->height},
                                  {"targets", report.screenshot->targets}};
    }
    if (report.window) {
        json["window"] = Json{{"drawableWidth", report.window->drawableWidth},
                              {"drawableHeight", report.window->drawableHeight}};
    }
    if (report.finalCamera) {
        json["finalCamera"] = spawnToJson(*report.finalCamera);
    }
    if (report.selfCheck) {
        json["selfCheck"] = Json{{"name", std::string(toString(report.selfCheck->check))},
                                 {"passed", report.selfCheck->passed},
                                 {"summary", report.selfCheck->summary}};
    }
    if (report.sceneContents) {
        json["sceneContents"] = Json{{"objects", report.sceneContents->objects},
                                     {"colliders", report.sceneContents->colliders},
                                     {"lights", report.sceneContents->lights}};
    }
    if (report.sceneAreas) {
        Json changes = Json::array();
        for (const SceneAreaChange& change : report.sceneAreas->changes) {
            changes.push_back(Json{{"frame", change.frame}, {"area", change.area}});
        }
        Json packed = Json::array();
        for (const PackedLightRecord& light : report.sceneAreas->packed) {
            packed.push_back(Json{{"name", light.name}, {"weight", light.weight}, {"clipped", light.clipped}});
        }
        const auto sampleJson = [](const SceneLightingSample& sample) {
            return Json{{"frame", sample.frame},
                        {"area", sample.area},
                        {"exposure", sample.exposure},
                        {"ambientIntensity", sample.ambientIntensity},
                        {"fillIntensity", sample.fillIntensity},
                        {"lights", sample.lights},
                        {"fading", sample.fading},
                        {"weight", sample.weight}};
        };
        Json samples = Json::array();
        for (const SceneLightingSample& sample : report.sceneAreas->samples) {
            samples.push_back(sampleJson(sample));
        }
        json["sceneAreas"] = Json{{"areas", report.sceneAreas->areas},
                                  {"area", report.sceneAreas->area},
                                  {"lights", report.sceneAreas->lights},
                                  {"keyLight", report.sceneAreas->keyLight},
                                  {"changes", std::move(changes)},
                                  {"packed", std::move(packed)},
                                  {"last", sampleJson(report.sceneAreas->last)},
                                  {"samples", std::move(samples)}};
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
    // The sections of I3: always written, empty ({}) in runs without what they record.
    json["world"] = worldToJson(report.world);
    json["residency"] = residencyToJson(report.residency);
    json["video"] = videoToJson(report.video);
    if (report.processPriority) {
        json["processPriority"] = processPriorityToJson(*report.processPriority);
    }
    if (report.crtLooks) {
        json["crtLooks"] = crtLooksToJson(*report.crtLooks);
    }
    json["screens"] = screensToJson(report.screens);
    json["directory"] = directoryToJson(report.directory);
    if (report.wizard) {
        json["wizard"] = wizardToJson(*report.wizard);
    }
    if (report.menu) {
        json["menu"] = menuToJson(*report.menu);
    }
    if (report.settings) {
        json["settings"] = settingsToJson(*report.settings);
    }
    if (report.controls) {
        json["controls"] = controlsToJson(*report.controls);
    }
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
    if (report.audio) {
        json["audio"] = audioToJson(*report.audio);
    }
    if (report.xr) {
        json["xr"] = xrToJson(*report.xr);
    }
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
                                             static_cast<int>(screenshot->count("height", extent)),
                                             screenshot->optionalString("targets").value_or(std::string())};
    }
    if (auto window = fields.optionalObject("window")) {
        const auto extent = static_cast<std::uint64_t>(std::numeric_limits<int>::max());
        report.window = WindowRecord{static_cast<int>(window->count("drawableWidth", extent)),
                                     static_cast<int>(window->count("drawableHeight", extent))};
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
    if (auto areas = fields.optionalObject("sceneAreas")) {
        SceneAreasRecord record;
        record.areas = static_cast<std::size_t>(areas->count("areas", kMaxSize));
        record.area = areas->string("area");
        record.lights = areas->stringArray("lights");
        record.keyLight = areas->string("keyLight");
        for (FieldReader& change : areas->objectArray("changes")) {
            record.changes.push_back(SceneAreaChange{change.count("frame"), change.string("area")});
        }
        for (FieldReader& light : areas->objectArray("packed")) {
            record.packed.push_back(
                PackedLightRecord{light.string("name"), light.number("weight"), light.boolean("clipped")});
        }
        const auto readSample = [](FieldReader& sample) {
            SceneLightingSample result;
            result.frame = sample.count("frame");
            result.area = sample.string("area");
            result.exposure = sample.number("exposure");
            result.ambientIntensity = sample.number("ambientIntensity");
            result.fillIntensity = sample.number("fillIntensity");
            result.lights = static_cast<std::size_t>(sample.count("lights", kMaxSize));
            result.fading = static_cast<std::size_t>(sample.count("fading", kMaxSize));
            result.weight = sample.number("weight");
            return result;
        };
        if (auto last = areas->object("last")) {
            record.last = readSample(*last);
        }
        for (FieldReader& sample : areas->objectArray("samples")) {
            record.samples.push_back(readSample(sample));
        }
        report.sceneAreas = std::move(record);
    }
    if (auto contents = fields.optionalObject("sceneContents")) {
        SceneContentsRecord record;
        record.objects = static_cast<std::size_t>(contents->count("objects", kMaxSize));
        record.colliders = static_cast<std::size_t>(contents->count("colliders", kMaxSize));
        record.lights = static_cast<std::size_t>(contents->count("lights", kMaxSize));
        report.sceneContents = record;
    }
    if (auto catalog = fields.optionalObject("catalog")) {
        report.catalog = readCatalog(*catalog);
    }
    for (FieldReader& launch : fields.optionalObjectArray("launches")) {
        report.launches.push_back(readLaunch(launch));
    }
    // The sections of I3 must be there (empty in runs without what they record).
    if (auto world = fields.object("world")) {
        report.world = readWorld(*world);
    }
    if (auto residency = fields.object("residency")) {
        report.residency = readResidency(*residency);
    }
    if (auto video = fields.object("video")) {
        report.video = readVideo(*video);
    }
    if (auto priority = fields.optionalObject("processPriority")) {
        report.processPriority = readProcessPriority(*priority);
    }
    if (auto looks = fields.optionalObject("crtLooks")) {
        report.crtLooks = readCrtLooks(*looks);
    }
    if (auto screens = fields.optionalObject("screens")) {
        report.screens = readScreens(*screens);
    }
    if (auto directory = fields.optionalObject("directory")) {
        report.directory = readDirectory(*directory);
    }
    if (auto wizard = fields.optionalObject("wizard")) {
        report.wizard = readWizard(*wizard);
    }
    if (auto menu = fields.optionalObject("menu")) {
        report.menu = readMenu(*menu);
    }
    if (auto settings = fields.optionalObject("settings")) {
        report.settings = readSettings(*settings);
    }
    if (auto controls = fields.optionalObject("controls")) {
        report.controls = readControls(*controls);
    }
    if (auto frameTimes = fields.object("frameTimes")) {
        report.frameTimes.warmUpFrames = frameTimes->count("warmUpFrames");
        report.frameTimes.times = readOptionalTimes(*frameTimes);
        report.frameTimes.gpuPasses = readGpuPasses(*frameTimes);
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
    if (auto audio = fields.optionalObject("audio")) {
        report.audio = readAudio(*audio);
    }
    if (auto xr = fields.optionalObject("xr")) {
        report.xr = readXr(*xr);
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
