#include <rv/scene/scene_description.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <optional>
#include <utility>

#include <glm/ext/matrix_transform.hpp>

#include <rv/core/json.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>

namespace rv::scene {
namespace {

using nlohmann::json;

/// Position inside the document, rendered as "<context>:<JSON pointer>" (RFC 6901 escaping).
class Location {
public:
    explicit Location(std::string_view context) : m_context(context) {}

    [[nodiscard]] Location key(std::string_view name) const {
        Location child = *this;
        child.m_pointer += '/';
        for (const char c : name) {
            if (c == '~') {
                child.m_pointer += "~0";
            } else if (c == '/') {
                child.m_pointer += "~1";
            } else {
                child.m_pointer += c;
            }
        }
        return child;
    }

    [[nodiscard]] Location index(std::size_t position) const {
        Location child = *this;
        child.m_pointer += '/';
        child.m_pointer += std::to_string(position);
        return child;
    }

    [[nodiscard]] std::string str() const {
        std::string text(m_context);
        text += ':';
        text += m_pointer.empty() ? std::string_view("/") : std::string_view(m_pointer);
        return text;
    }

    [[nodiscard]] const std::string& pointer() const noexcept { return m_pointer; }

private:
    std::string_view m_context;
    std::string m_pointer;
};

Error validationError(const Location& at, std::string message) {
    return Error{ErrorCode::ValidationFailed, std::move(message), at.str()};
}

tl::unexpected<Error> fail(const Location& at, std::string message) {
    return propagate(validationError(at, std::move(message)));
}

std::string joinNames(std::initializer_list<std::string_view> names) {
    std::string text;
    for (const std::string_view name : names) {
        if (!text.empty()) {
            text += ", ";
        }
        text += name;
    }
    return text;
}

// --- Value readers: Expected<T>(const json&, const Location&) ----------------------------------------------

Expected<float> readNumber(const json& value, const Location& at) {
    if (!value.is_number()) {
        return fail(at, "expected a number");
    }
    const double number = value.get<double>();
    if (!std::isfinite(number) || std::fabs(number) > static_cast<double>(std::numeric_limits<float>::max())) {
        return fail(at, "number out of range");
    }
    return static_cast<float>(number);
}

/// Non-negative integer up to `maximum` (accepts signed and unsigned JSON integers, not 3.0).
Expected<std::uint64_t> readCount(const json& value, const Location& at, std::uint64_t maximum) {
    std::optional<std::uint64_t> number;
    if (value.is_number_unsigned()) {
        number = value.get<std::uint64_t>();
    } else if (value.is_number_integer() && value.get<std::int64_t>() >= 0) {
        number = static_cast<std::uint64_t>(value.get<std::int64_t>());
    }
    if (!number) {
        return fail(at, "expected a non-negative integer");
    }
    if (*number > maximum) {
        return fail(at, "integer larger than " + std::to_string(maximum));
    }
    return *number;
}

Expected<std::uint32_t> readSeed(const json& value, const Location& at) {
    auto seed = readCount(value, at, std::numeric_limits<std::uint32_t>::max());
    if (!seed) {
        return propagate(seed.error());
    }
    return static_cast<std::uint32_t>(*seed);
}

Expected<bool> readBool(const json& value, const Location& at) {
    if (!value.is_boolean()) {
        return fail(at, "expected true or false");
    }
    return value.get<bool>();
}

Expected<std::string> readString(const json& value, const Location& at) {
    if (!value.is_string()) {
        return fail(at, "expected a string");
    }
    return value.get<std::string>();
}

Expected<std::string> readNonEmptyString(const json& value, const Location& at) {
    auto text = readString(value, at);
    if (text && text->empty()) {
        return fail(at, "must not be empty");
    }
    return text;
}

Expected<glm::vec3> readVec3(const json& value, const Location& at) {
    if (!value.is_array() || value.size() != 3) {
        return fail(at, "expected an array of 3 numbers");
    }
    glm::vec3 result(0.0f);
    for (std::size_t i = 0; i < 3; ++i) {
        auto component = readNumber(value[i], at.index(i));
        if (!component) {
            return propagate(component.error());
        }
        result[static_cast<glm::length_t>(i)] = *component;
    }
    return result;
}

/// sRGB hex string -> linear RGB.
Expected<glm::vec3> readColor(const json& value, const Location& at) {
    if (!value.is_string()) {
        return fail(at, "expected an sRGB hex color string such as \"#FFC88A\"");
    }
    const auto& text = value.get_ref<const std::string&>();
    auto color = core::hexToLinearRgb(text);
    if (!color) {
        return fail(at, "invalid color \"" + text + "\": " + color.error().message);
    }
    return *color;
}

Expected<glm::vec3> readScale(const json& value, const Location& at) {
    glm::vec3 scale(1.0f);
    if (value.is_number()) {
        auto uniform = readNumber(value, at);
        if (!uniform) {
            return propagate(uniform.error());
        }
        scale = glm::vec3(*uniform);
    } else if (value.is_array()) {
        auto vector = readVec3(value, at);
        if (!vector) {
            return propagate(vector.error());
        }
        scale = *vector;
    } else {
        return fail(at, "expected a number or an array of 3 numbers");
    }
    if (!(scale.x > 0.0f && scale.y > 0.0f && scale.z > 0.0f)) {
        return fail(at, "scale components must be > 0");
    }
    return scale;
}

Expected<json> readJsonObject(const json& value, const Location& at) {
    if (!value.is_object()) {
        return fail(at, "expected an object");
    }
    return value;
}

/// Asset-relative path: '/' separators, no root, drive, empty or ".." segment.
Expected<std::string> readAssetPath(const json& value, const Location& at) {
    auto path = readNonEmptyString(value, at);
    if (!path) {
        return path;
    }
    if (path->find('\\') != std::string::npos) {
        return fail(at, "asset paths use '/' separators: \"" + *path + "\"");
    }
    if (path->front() == '/' || path->find(':') != std::string::npos) {
        return fail(at, "asset paths must be relative to the asset root: \"" + *path + "\"");
    }
    for (const std::string_view segment : core::split(*path, '/')) {
        if (segment.empty() || segment == "..") {
            return fail(at, "invalid asset path segment in \"" + *path + "\"");
        }
    }
    return path;
}

// --- Object field access ----------------------------------------------------------------------------------

/// Reads the fields of one JSON object. The first error is recorded and every later call becomes a no-op, so
/// a parser reads all fields in sequence and checks failed() once at the end.
class Fields {
public:
    Fields(const json& value, Location at, std::initializer_list<std::string_view> allowedKeys) : m_at(std::move(at)) {
        if (!value.is_object()) {
            m_error = validationError(m_at, "expected an object");
            return;
        }
        m_object = &value;
        for (const auto& item : value.items()) {
            const std::string& name = item.key();
            if (std::find(allowedKeys.begin(), allowedKeys.end(), name) == allowedKeys.end()) {
                m_error = validationError(m_at.key(name),
                                          "unknown key \"" + name + "\" (allowed: " + joinNames(allowedKeys) + ")");
                return;
            }
        }
    }

    [[nodiscard]] bool failed() const noexcept { return m_error.has_value(); }
    [[nodiscard]] const Error& error() const { return *m_error; }
    [[nodiscard]] bool has(std::string_view name) const { return m_object != nullptr && m_object->contains(name); }
    /// The value of `name` when present and no error is pending, else nullptr.
    [[nodiscard]] const json* get(std::string_view name) const {
        return !failed() && has(name) ? &m_object->at(name) : nullptr;
    }

    /// Reads `name` into `out` when present; returns true when a value was read.
    template <class T, class Reader>
    bool optional(std::string_view name, T& out, Reader&& reader) {
        if (failed() || !has(name)) {
            return false;
        }
        auto parsed = reader(m_object->at(name), m_at.key(name));
        if (!parsed) {
            m_error = std::move(parsed.error());
            return false;
        }
        out = std::move(*parsed);
        return true;
    }

    template <class T, class Reader>
    void required(std::string_view name, T& out, Reader&& reader) {
        if (!failed() && !has(name)) {
            failAt(m_at.key(name), "missing required key \"" + std::string(name) + "\"");
        }
        optional(name, out, std::forward<Reader>(reader));
    }

    /// Records an error at `/<name>` when `condition` is false.
    void check(bool condition, std::string_view name, std::string message) {
        if (!condition) {
            failAt(m_at.key(name), std::move(message));
        }
    }

    void failAt(const Location& at, std::string message) {
        if (!failed()) {
            m_error = validationError(at, std::move(message));
        }
    }

private:
    const json* m_object = nullptr;
    Location m_at;
    std::optional<Error> m_error;
};

std::string formatNumber(float value) {
    std::string text = std::to_string(value);
    // std::to_string prints six decimals; trim the zeros for readable messages.
    while (text.size() > 1 && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

// --- Sections -----------------------------------------------------------------------------------------------

Expected<CameraSetup> parseCamera(const json& value, const Location& at) {
    Fields fields(value, at, {"position", "yawDeg", "pitchDeg", "horizontalFovDeg", "near", "far"});
    CameraSetup camera;
    fields.optional("position", camera.position, readVec3);
    fields.optional("yawDeg", camera.yawDeg, readNumber);
    fields.optional("pitchDeg", camera.pitchDeg, readNumber);
    fields.check(std::fabs(camera.pitchDeg) <= 89.0f, "pitchDeg", "must be within [-89, 89] degrees");
    fields.optional("horizontalFovDeg", camera.lens.horizontalFovDeg, readNumber);
    fields.check(camera.lens.horizontalFovDeg >= Camera::kMinHorizontalFovDeg &&
                     camera.lens.horizontalFovDeg <= Camera::kMaxHorizontalFovDeg,
                 "horizontalFovDeg", "must be within [1, 179] degrees");
    fields.optional("near", camera.lens.nearPlane, readNumber);
    fields.check(camera.lens.nearPlane > 0.0f, "near", "must be > 0");
    fields.optional("far", camera.lens.farPlane, readNumber);
    fields.check(camera.lens.farPlane > camera.lens.nearPlane, "far", "must be greater than near");
    if (fields.failed()) {
        return propagate(fields.error());
    }
    return camera;
}

Expected<EnvironmentSetup> parseEnvironment(const json& value, const Location& at) {
    Fields fields(value, at, {"clearColor", "ambientSky", "ambientGround", "ambientIntensity", "exposure"});
    EnvironmentSetup environment;
    fields.optional("clearColor", environment.clearColor, readColor);
    fields.optional("ambientSky", environment.ambientSky, readColor);
    fields.optional("ambientGround", environment.ambientGround, readColor);
    fields.optional("ambientIntensity", environment.ambientIntensity, readNumber);
    fields.check(environment.ambientIntensity >= 0.0f, "ambientIntensity", "must be >= 0");
    fields.optional("exposure", environment.exposure, readNumber);
    fields.check(environment.exposure > 0.0f, "exposure", "must be > 0");
    if (fields.failed()) {
        return propagate(fields.error());
    }
    return environment;
}

Expected<Light> parseLight(const json& value, const Location& at) {
    Fields fields(value, at,
                  {"name", "type", "position", "direction", "target", "color", "intensity", "range", "innerConeDeg",
                   "outerConeDeg", "castsShadows"});
    Light light;
    fields.optional("name", light.name, readString);

    std::string typeName;
    fields.required("type", typeName, readString);
    if (!fields.failed()) {
        const auto type = lightTypeFromString(typeName);
        fields.check(type.has_value(), "type",
                     "unknown light type \"" + typeName + "\" (expected point, spot or directional)");
        light.type = type.value_or(LightType::Point);
    }
    const bool positional = light.type != LightType::Directional;
    if (positional) {
        fields.required("position", light.position, readVec3);
    } else {
        fields.optional("position", light.position, readVec3);
    }

    glm::vec3 direction(0.0f);
    glm::vec3 target(0.0f);
    const bool hasDirection = fields.optional("direction", direction, readVec3);
    const bool hasTarget = fields.optional("target", target, readVec3);
    fields.check(!(hasDirection && hasTarget), "target", "\"direction\" and \"target\" are mutually exclusive");
    if (hasTarget) {
        direction = target - light.position;
        fields.check(glm::length(direction) > 1e-6f, "target", "target must differ from the light position");
    } else if (hasDirection) {
        fields.check(glm::length(direction) > 1e-6f, "direction", "direction must not be zero");
    } else {
        fields.check(light.type == LightType::Point, "direction",
                     std::string(toString(light.type)) + " lights need \"direction\" or \"target\"");
    }
    if (!fields.failed() && (hasDirection || hasTarget)) {
        light.direction = glm::normalize(direction);
    }

    fields.optional("color", light.color, readColor);
    fields.optional("intensity", light.intensity, readNumber);
    fields.check(light.intensity >= 0.0f, "intensity", "must be >= 0");
    fields.optional("range", light.range, readNumber);
    fields.check(!positional || light.range > 0.0f, "range", "must be > 0 for point and spot lights");
    fields.optional("innerConeDeg", light.innerConeDeg, readNumber);
    fields.optional("outerConeDeg", light.outerConeDeg, readNumber);
    fields.check(light.innerConeDeg >= 0.0f, "innerConeDeg", "must be >= 0");
    fields.check(light.innerConeDeg <= light.outerConeDeg, "innerConeDeg",
                 "innerConeDeg (" + formatNumber(light.innerConeDeg) + ") must not exceed outerConeDeg (" +
                     formatNumber(light.outerConeDeg) + ")");
    fields.check(light.outerConeDeg < 90.0f, "outerConeDeg", "must be < 90");
    fields.optional("castsShadows", light.castsShadows, readBool);
    if (fields.failed()) {
        return propagate(fields.error());
    }
    return light;
}

Expected<TextureSource> parseTexture(const json& value, const Location& at) {
    if (!value.is_object() || value.contains("file") == value.contains("pattern")) {
        return fail(at, "expected an object with exactly one of \"pattern\" or \"file\"");
    }
    TextureSource texture;
    if (value.contains("file")) {
        Fields fields(value, at, {"file"});
        texture.kind = TextureSource::Kind::File;
        fields.required("file", texture.path, readAssetPath);
        if (fields.failed()) {
            return propagate(fields.error());
        }
        return texture;
    }
    Fields fields(value, at, {"pattern", "title", "seed"});
    texture.kind = TextureSource::Kind::Pattern;
    fields.required("pattern", texture.pattern, readString);
    if (!fields.failed()) {
        const bool known = std::find(kTexturePatternNames.begin(), kTexturePatternNames.end(), texture.pattern) !=
                           kTexturePatternNames.end();
        fields.check(known, "pattern",
                     "unknown texture pattern \"" + texture.pattern + "\" (expected color_bars, checker or cover)");
    }
    fields.optional("title", texture.title, readString);
    fields.optional("seed", texture.seed, readSeed);
    if (fields.failed()) {
        return propagate(fields.error());
    }
    return texture;
}

Expected<std::map<std::string, std::string>> readMaterialMap(const json& value, const Location& at) {
    if (!value.is_object()) {
        return fail(at, "expected an object mapping material roles to material names");
    }
    std::map<std::string, std::string> materials;
    for (const auto& item : value.items()) {
        const Location roleAt = at.key(item.key());
        if (item.key().empty()) {
            return fail(roleAt, "material role names must not be empty");
        }
        auto name = readNonEmptyString(item.value(), roleAt);
        if (!name) {
            return propagate(name.error());
        }
        materials.emplace(item.key(), std::move(*name));
    }
    return materials;
}

Expected<std::map<std::string, TextureSource>> readTextureMap(const json& value, const Location& at) {
    if (!value.is_object()) {
        return fail(at, "expected an object mapping material roles to textures");
    }
    std::map<std::string, TextureSource> textures;
    for (const auto& item : value.items()) {
        const Location roleAt = at.key(item.key());
        if (item.key().empty()) {
            return fail(roleAt, "material role names must not be empty");
        }
        auto texture = parseTexture(item.value(), roleAt);
        if (!texture) {
            return propagate(texture.error());
        }
        textures.emplace(item.key(), std::move(*texture));
    }
    return textures;
}

struct GridSpec {
    std::array<std::uint64_t, 3> counts{1, 1, 1};
    glm::vec3 spacing{0.0f};

    [[nodiscard]] std::uint64_t total() const noexcept { return counts[0] * counts[1] * counts[2]; }
};

Expected<std::array<std::uint64_t, 3>> readGridCounts(const json& value, const Location& at) {
    if (!value.is_array() || value.size() != 3) {
        return fail(at, "expected an array of 3 integers [nx, ny, nz]");
    }
    std::array<std::uint64_t, 3> counts{};
    for (std::size_t i = 0; i < 3; ++i) {
        auto count = readCount(value[i], at.index(i), kMaxSceneInstances);
        if (!count) {
            return propagate(count.error());
        }
        if (*count < 1) {
            return fail(at.index(i), "grid counts must be >= 1");
        }
        counts[i] = *count;
    }
    return counts;
}

Expected<GridSpec> readGrid(const json& value, const Location& at) {
    Fields fields(value, at, {"counts", "spacing"});
    GridSpec grid;
    fields.required("counts", grid.counts, readGridCounts);
    fields.required("spacing", grid.spacing, readVec3);
    if (fields.failed()) {
        return propagate(fields.error());
    }
    return grid;
}

glm::mat4 composeTransform(const glm::vec3& position, const glm::vec3& rotationDeg, const glm::vec3& scale) {
    const glm::mat4 identity(1.0f);
    const glm::mat4 rotation =
        glm::rotate(identity, core::degreesToRadians(rotationDeg.y), glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::rotate(identity, core::degreesToRadians(rotationDeg.x), glm::vec3(1.0f, 0.0f, 0.0f)) *
        glm::rotate(identity, core::degreesToRadians(rotationDeg.z), glm::vec3(0.0f, 0.0f, 1.0f));
    return glm::translate(identity, position) * rotation * glm::scale(identity, scale);
}

/// Parses one entry of "objects". `instancesLeft` is the remaining scene-wide instance budget.
Expected<SceneObject> parseObject(const json& value, const Location& at, std::size_t& instancesLeft) {
    Fields fields(value, at,
                  {"id", "prefab", "primitive", "model", "params", "position", "rotationDeg", "scale", "material",
                   "materials", "textures", "inlineMaterial", "grid", "castsShadows", "collides"});
    SceneObject object;
    fields.required("id", object.id, readNonEmptyString);

    const int sourceCount =
        (fields.has("prefab") ? 1 : 0) + (fields.has("primitive") ? 1 : 0) + (fields.has("model") ? 1 : 0);
    if (sourceCount != 1) {
        fields.failAt(at, sourceCount == 0 ? "object needs one of \"prefab\", \"primitive\" or \"model\""
                                           : "object must have exactly one of \"prefab\", \"primitive\" or \"model\"");
    }
    if (fields.optional("prefab", object.kind, readNonEmptyString)) {
        object.source = SceneObject::Source::Prefab;
    } else if (fields.optional("primitive", object.kind, readNonEmptyString)) {
        object.source = SceneObject::Source::Primitive;
    } else if (fields.optional("model", object.modelPath, readAssetPath)) {
        object.source = SceneObject::Source::Model;
    }
    fields.optional("params", object.params, readJsonObject);

    glm::vec3 position(0.0f);
    glm::vec3 rotationDeg(0.0f);
    glm::vec3 scale(1.0f);
    fields.optional("position", position, readVec3);
    fields.optional("rotationDeg", rotationDeg, readVec3);
    fields.optional("scale", scale, readScale);
    object.transform = composeTransform(position, rotationDeg, scale);

    fields.optional("material", object.material, readNonEmptyString);
    fields.optional("materials", object.materialsByRole, readMaterialMap);
    fields.optional("textures", object.texturesByRole, readTextureMap);
    fields.optional("inlineMaterial", object.inlineMaterial, readJsonObject);
    fields.check(!(fields.has("material") && fields.has("inlineMaterial")), "inlineMaterial",
                 "\"material\" and \"inlineMaterial\" are mutually exclusive");

    GridSpec grid;
    const bool hasGrid = fields.optional("grid", grid, readGrid);
    const std::uint64_t instanceCount = hasGrid ? grid.total() : 1;
    if (instanceCount > instancesLeft) {
        fields.failAt(hasGrid ? at.key("grid").key("counts") : at,
                      "scene exceeds " + std::to_string(kMaxSceneInstances) + " instances (this object adds " +
                          std::to_string(instanceCount) + ", " + std::to_string(instancesLeft) + " left)");
    }
    fields.optional("castsShadows", object.castsShadows, readBool);
    fields.optional("collides", object.collides, readBool);
    if (fields.failed()) {
        return propagate(fields.error());
    }

    instancesLeft -= static_cast<std::size_t>(instanceCount);
    if (hasGrid) {
        object.instances.reserve(static_cast<std::size_t>(instanceCount));
        for (std::uint64_t iz = 0; iz < grid.counts[2]; ++iz) {
            for (std::uint64_t iy = 0; iy < grid.counts[1]; ++iy) {
                for (std::uint64_t ix = 0; ix < grid.counts[0]; ++ix) {
                    const glm::vec3 offset(static_cast<float>(ix) * grid.spacing.x,
                                           static_cast<float>(iy) * grid.spacing.y,
                                           static_cast<float>(iz) * grid.spacing.z);
                    object.instances.push_back(object.transform * glm::translate(glm::mat4(1.0f), offset));
                }
            }
        }
    }
    return object;
}

Expected<SceneDescription> parseDocument(const json& document, const Location& root) {
    Fields fields(document, root, {"schemaVersion", "name", "camera", "environment", "lights", "objects"});
    std::uint64_t version = 0;
    fields.required("schemaVersion", version, [](const json& value, const Location& at) {
        return readCount(value, at, std::numeric_limits<std::uint32_t>::max());
    });
    fields.check(version == static_cast<std::uint64_t>(kSceneSchemaVersion), "schemaVersion",
                 "unsupported schemaVersion " + std::to_string(version) +
                     " (supported: " + std::to_string(kSceneSchemaVersion) + ")");

    SceneDescription scene;
    fields.required("name", scene.name, readNonEmptyString);
    fields.optional("camera", scene.camera, parseCamera);
    fields.optional("environment", scene.environment, parseEnvironment);

    const json* lights = fields.get("lights");
    fields.check(lights == nullptr || lights->is_array(), "lights", "expected an array");
    const json* objects = fields.get("objects");
    fields.check(objects == nullptr || objects->is_array(), "objects", "expected an array");
    if (fields.failed()) {
        return propagate(fields.error());
    }

    const Location lightsAt = root.key("lights");
    const std::size_t lightCount = lights != nullptr ? lights->size() : 0;
    scene.lights.reserve(lightCount);
    for (std::size_t i = 0; i < lightCount; ++i) {
        auto light = parseLight((*lights)[i], lightsAt.index(i));
        if (!light) {
            return propagate(light.error());
        }
        scene.lights.push_back(std::move(*light));
    }

    const Location objectsAt = root.key("objects");
    const std::size_t objectCount = objects != nullptr ? objects->size() : 0;
    std::map<std::string, std::size_t> firstIndexById;
    std::size_t instancesLeft = kMaxSceneInstances;
    scene.objects.reserve(objectCount);
    for (std::size_t i = 0; i < objectCount; ++i) {
        auto object = parseObject((*objects)[i], objectsAt.index(i), instancesLeft);
        if (!object) {
            return propagate(object.error());
        }
        const auto [first, inserted] = firstIndexById.emplace(object->id, i);
        if (!inserted) {
            return fail(objectsAt.index(i).key("id"), "duplicate object id \"" + object->id + "\" (first used by " +
                                                          objectsAt.index(first->second).pointer() + ")");
        }
        scene.objects.push_back(std::move(*object));
    }
    return scene;
}

} // namespace

std::span<const glm::mat4> SceneObject::instanceTransforms() const noexcept {
    if (instances.empty()) {
        return {&transform, 1};
    }
    return instances;
}

std::string_view SceneObject::materialNameForRole(std::string_view role) const {
    const auto it = materialsByRole.find(std::string(role));
    return it != materialsByRole.end() ? std::string_view(it->second) : std::string_view(material);
}

const SceneObject* SceneDescription::findObject(std::string_view id) const noexcept {
    const auto it =
        std::find_if(objects.begin(), objects.end(), [id](const SceneObject& object) { return object.id == id; });
    return it != objects.end() ? &*it : nullptr;
}

Expected<SceneDescription> parseSceneDescription(const nlohmann::json& document, std::string_view context) {
    try {
        return parseDocument(document, Location(context));
    } catch (const std::exception& e) {
        // Defensive boundary: readers check types before every access, so this only catches allocation failures
        // or library errors and keeps exceptions from crossing the module boundary.
        return makeError(ErrorCode::ValidationFailed, std::string("cannot read scene description: ") + e.what(),
                         std::string(context));
    }
}

Expected<SceneDescription> loadSceneDescription(const std::filesystem::path& path) {
    auto document = core::readJsonFile(path);
    if (!document) {
        return propagate(document.error());
    }
    return parseSceneDescription(*document, core::pathToUtf8(path.filename()));
}

} // namespace rv::scene
