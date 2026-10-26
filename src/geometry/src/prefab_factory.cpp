#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/json.hpp>
#include <rv/geometry/mesh_builder.hpp>
#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using Json = nlohmann::json;
using detail::checkRange;
using detail::firstError;

/// Reads the parameters of one kind from a JSON object (or null) and keeps the first error. Missing keys leave
/// the output untouched (defaults); error contexts are "<prefix>.<key>".
class ParamReader {
public:
    ParamReader(std::string prefix, const Json& params, std::initializer_list<std::string_view> keys)
        : m_prefix(std::move(prefix)) {
        if (params.is_null()) {
            return;
        }
        if (!params.is_object()) {
            fail(ErrorCode::InvalidArgument, "parameters must be a JSON object or null", m_prefix);
            return;
        }
        m_object = &params;
        // nlohmann::json objects iterate in key order: the reported unknown key is deterministic.
        for (auto it = params.begin(); it != params.end(); ++it) {
            if (std::find(keys.begin(), keys.end(), std::string_view(it.key())) == keys.end()) {
                fail(ErrorCode::InvalidArgument, "unknown parameter", context(it.key()));
                return;
            }
        }
    }

    [[nodiscard]] bool ok() const noexcept { return !m_error.has_value(); }
    [[nodiscard]] const Error& error() const noexcept { return *m_error; }

    /// The value of `key`, or nullptr when it is absent or an error was already recorded.
    [[nodiscard]] const Json* find(std::string_view key) const {
        if (!ok() || m_object == nullptr) {
            return nullptr;
        }
        const auto it = m_object->find(std::string(key));
        return it == m_object->end() ? nullptr : &*it;
    }

    void read(std::string_view key, float& out) {
        const Json* value = find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->is_number()) {
            fail(ErrorCode::InvalidArgument, "expected a number", context(key));
            return;
        }
        const double number = value->get<double>();
        if (!std::isfinite(number) || std::fabs(number) > static_cast<double>(std::numeric_limits<float>::max())) {
            fail(ErrorCode::OutOfRange, "expected a finite number", context(key));
            return;
        }
        out = static_cast<float>(number);
    }

    void read(std::string_view key, int& out) {
        const Json* value = find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->is_number_integer()) {
            fail(ErrorCode::InvalidArgument, "expected an integer", context(key));
            return;
        }
        constexpr auto kMax = static_cast<std::int64_t>(std::numeric_limits<int>::max());
        constexpr auto kMin = static_cast<std::int64_t>(std::numeric_limits<int>::min());
        if (value->is_number_unsigned() && value->get<std::uint64_t>() > static_cast<std::uint64_t>(kMax)) {
            fail(ErrorCode::OutOfRange, "integer too large", context(key));
            return;
        }
        const auto number = value->get<std::int64_t>();
        if (number < kMin || number > kMax) {
            fail(ErrorCode::OutOfRange, "integer out of range", context(key));
            return;
        }
        out = static_cast<int>(number);
    }

    /// A number, or null for "none".
    void read(std::string_view key, std::optional<float>& out) {
        const Json* value = find(key);
        if (value == nullptr) {
            return;
        }
        if (value->is_null()) {
            out.reset();
            return;
        }
        float number = 0.0f;
        read(key, number);
        if (ok()) {
            out = number;
        }
    }

    /// An array of integers in int range.
    void read(std::string_view key, std::vector<int>& out) {
        const Json* value = find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->is_array()) {
            fail(ErrorCode::InvalidArgument, "expected an array of integers", context(key));
            return;
        }
        std::vector<int> numbers;
        for (const Json& element : *value) {
            if (!element.is_number_integer() ||
                (element.is_number_unsigned() &&
                 element.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) ||
                (!element.is_number_unsigned() && (element.get<std::int64_t>() < std::numeric_limits<int>::min() ||
                                                   element.get<std::int64_t>() > std::numeric_limits<int>::max()))) {
                fail(ErrorCode::InvalidArgument, "expected an array of integers", context(key));
                return;
            }
            numbers.push_back(static_cast<int>(element.get<std::int64_t>()));
        }
        out = std::move(numbers);
    }

    void read(std::string_view key, bool& out) {
        const Json* value = find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->is_boolean()) {
            fail(ErrorCode::InvalidArgument, "expected true or false", context(key));
            return;
        }
        out = value->get<bool>();
    }

    void read(std::string_view key, glm::vec2& out) {
        if (const Json* value = find(key)) {
            store(core::jsonToVec2(*value, context(key)), out, key);
        }
    }

    void read(std::string_view key, glm::vec3& out) {
        if (const Json* value = find(key)) {
            store(core::jsonToVec3(*value, context(key)), out, key);
        }
    }

    /// String parameter mapped through `names` (JSON spelling -> value).
    template <class T>
    void readEnum(std::string_view key, T& out, std::initializer_list<std::pair<std::string_view, T>> names) {
        const Json* value = find(key);
        if (value == nullptr) {
            return;
        }
        std::string expected;
        if (value->is_string()) {
            const std::string& text = value->get_ref<const std::string&>();
            for (const auto& [name, mapped] : names) {
                if (text == name) {
                    out = mapped;
                    return;
                }
            }
        }
        for (const auto& entry : names) {
            expected += expected.empty() ? "\"" : ", \"";
            expected += entry.first;
            expected += '"';
        }
        fail(ErrorCode::InvalidArgument, "expected one of " + expected, context(key));
    }

    void fail(ErrorCode code, std::string message, std::string where) {
        if (ok()) {
            m_error = Error{code, std::move(message), std::move(where)};
        }
    }

    [[nodiscard]] std::string context(std::string_view key) const {
        std::string text = m_prefix;
        text += '.';
        text += key;
        return text;
    }

private:
    template <class Vector>
    void store(const Expected<Vector>& parsed, Vector& out, std::string_view key) {
        if (!parsed) {
            fail(ErrorCode::InvalidArgument, parsed.error().message, context(key));
            return;
        }
        for (int i = 0; i < Vector::length(); ++i) {
            if (!std::isfinite((*parsed)[i])) {
                fail(ErrorCode::OutOfRange, "expected finite numbers", context(key));
                return;
            }
        }
        out = *parsed;
    }

    std::string m_prefix;
    const Json* m_object = nullptr;
    std::optional<Error> m_error;
};

// --- Prefabs -------------------------------------------------------------------------------------------------

Expected<ProceduralPrefab> roomShellFromJson(const Json& params) {
    RoomShellParams p;
    ParamReader r("room_shell", params,
                  {"size", "wallHeight", "wallThickness", "metersPerTile", "ceiling", "baseboardHeight",
                   "wainscotHeight", "doorway"});
    r.read("size", p.size);
    r.read("wallHeight", p.wallHeight);
    r.read("wallThickness", p.wallThickness);
    r.read("metersPerTile", p.metersPerTile);
    r.read("ceiling", p.ceiling);
    r.read("baseboardHeight", p.baseboardHeight);
    r.read("wainscotHeight", p.wainscotHeight);
    if (const Json* doorway = r.find("doorway")) {
        if (doorway->is_null()) {
            p.doorway.reset();
        } else {
            DoorwayParams door;
            ParamReader d("room_shell.doorway", *doorway, {"wall", "offset", "width", "height", "stop"});
            d.readEnum("wall", door.wall,
                       {{"north", WallSide::North},
                        {"south", WallSide::South},
                        {"east", WallSide::East},
                        {"west", WallSide::West}});
            d.read("offset", door.offset);
            d.read("width", door.width);
            d.read("height", door.height);
            d.read("stop", door.stop);
            if (!d.ok()) {
                return propagate(d.error());
            }
            p.doorway = door;
        }
    }
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildRoomShell(p);
}

Expected<ProceduralPrefab> shelfFromJson(const Json& params) {
    ShelfParams p;
    ParamReader r("shelf", params,
                  {"width", "height", "depth", "levels", "boardThickness", "back", "plinthHeight", "slotSize",
                   "slotGap", "slotMargin", "slotFrontInset", "displayLevels", "displaySlotSize", "displaySlotGap",
                   "displaySlotMargin", "boardLight"});
    r.read("width", p.width);
    r.read("height", p.height);
    r.read("depth", p.depth);
    r.read("levels", p.levels);
    r.read("boardThickness", p.boardThickness);
    r.read("back", p.back);
    r.read("plinthHeight", p.plinthHeight);
    r.read("slotSize", p.slotSize);
    r.read("slotGap", p.slotGap);
    r.read("slotMargin", p.slotMargin);
    r.read("slotFrontInset", p.slotFrontInset);
    r.read("displayLevels", p.displayLevels);
    r.read("displaySlotSize", p.displaySlotSize);
    r.read("displaySlotGap", p.displaySlotGap);
    r.read("displaySlotMargin", p.displaySlotMargin);
    r.read("boardLight", p.boardLight);
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildShelf(p);
}

Expected<ProceduralPrefab> cartridgeFromJson(const Json& params) {
    CartridgeParams p;
    ParamReader r(
        "cartridge", params,
        {"size", "chamfer", "labelMargin", "labelInset", "gripRidges", "spineLabel", "spineLabelSize", "spineFace"});
    r.read("size", p.size);
    r.read("chamfer", p.chamfer);
    r.read("labelMargin", p.labelMargin);
    r.read("labelInset", p.labelInset);
    r.read("gripRidges", p.gripRidges);
    r.read("spineLabel", p.spineLabel);
    r.read("spineLabelSize", p.spineLabelSize);
    r.readEnum("spineFace", p.spineFace, {{"top", SpineFace::Top}, {"side", SpineFace::Side}});
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildCartridge(p);
}

Expected<ProceduralPrefab> crtFromJson(const Json& params) {
    CrtParams p;
    ParamReader r("crt", params, {"width", "height", "depth", "screenAspect", "bezel", "screenInset", "backTaper"});
    r.read("width", p.width);
    r.read("height", p.height);
    r.read("depth", p.depth);
    r.read("screenAspect", p.screenAspect);
    r.read("bezel", p.bezel);
    r.read("screenInset", p.screenInset);
    r.read("backTaper", p.backTaper);
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildCrt(p);
}

Expected<ProceduralPrefab> tvCabinetFromJson(const Json& params) {
    TvCabinetParams p;
    ParamReader r("tv_cabinet", params, {"width", "height", "depth", "drawers", "legHeight"});
    r.read("width", p.width);
    r.read("height", p.height);
    r.read("depth", p.depth);
    r.read("drawers", p.drawers);
    r.read("legHeight", p.legHeight);
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildTvCabinet(p);
}

Expected<ProceduralPrefab> consoleCartridgeFromJson(const Json& params) {
    ConsoleCartridgeParams p;
    ParamReader r("console_cartridge", params, {"width", "height", "depth"});
    r.read("width", p.width);
    r.read("height", p.height);
    r.read("depth", p.depth);
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildConsoleCartridge(p);
}

Expected<ProceduralPrefab> posterFrameFromJson(const Json& params) {
    PosterFrameParams p;
    ParamReader r("poster_frame", params, {"width", "height", "frameWidth", "depth"});
    r.read("width", p.width);
    r.read("height", p.height);
    r.read("frameWidth", p.frameWidth);
    r.read("depth", p.depth);
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildPosterFrame(p);
}

Expected<ProceduralPrefab> rugFromJson(const Json& params) {
    RugParams p;
    ParamReader r("rug", params, {"width", "depth", "thickness"});
    r.read("width", p.width);
    r.read("depth", p.depth);
    r.read("thickness", p.thickness);
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildRug(p);
}

Expected<ProceduralPrefab> plantFromJson(const Json& params) {
    PlantParams p;
    ParamReader r("plant", params, {"height", "potRadius", "potHeight", "spread", "leaves", "seed", "saucerRadius"});
    r.read("height", p.height);
    r.read("potRadius", p.potRadius);
    r.read("potHeight", p.potHeight);
    r.read("spread", p.spread);
    r.read("leaves", p.leaves);
    r.read("seed", p.seed);
    r.read("saucerRadius", p.saucerRadius);
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildPlant(p);
}

Expected<ProceduralPrefab> floorLampFromJson(const Json& params) {
    FloorLampParams p;
    ParamReader r("floor_lamp", params, {"height", "baseRadius", "shadeRadius", "shadeHeight", "shade"});
    r.read("height", p.height);
    r.read("baseRadius", p.baseRadius);
    r.read("shadeRadius", p.shadeRadius);
    r.read("shadeHeight", p.shadeHeight);
    r.readEnum("shade", p.shade, {{"drum", LampShade::Drum}, {"torchiere", LampShade::Torchiere}});
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildFloorLamp(p);
}

Expected<ProceduralPrefab> armchairFromJson(const Json& params) {
    ArmchairParams p;
    ParamReader r("armchair", params, {"width", "depth", "height", "seatHeight", "armHeight", "legHeight"});
    r.read("width", p.width);
    r.read("depth", p.depth);
    r.read("height", p.height);
    r.read("seatHeight", p.seatHeight);
    r.read("armHeight", p.armHeight);
    r.read("legHeight", p.legHeight);
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildArmchair(p);
}

Expected<ProceduralPrefab> sofaFromJson(const Json& params) {
    SofaParams p;
    ParamReader r("sofa", params,
                  {"width", "depth", "height", "seatHeight", "armHeight", "legHeight", "seats", "pillows"});
    r.read("width", p.width);
    r.read("depth", p.depth);
    r.read("height", p.height);
    r.read("seatHeight", p.seatHeight);
    r.read("armHeight", p.armHeight);
    r.read("legHeight", p.legHeight);
    r.read("seats", p.seats);
    r.read("pillows", p.pillows);
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildSofa(p);
}

Expected<ProceduralPrefab> beanbagFromJson(const Json& params) {
    BeanbagParams p;
    ParamReader r("beanbag", params, {"width", "depth", "height"});
    r.read("width", p.width);
    r.read("depth", p.depth);
    r.read("height", p.height);
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildBeanbag(p);
}

Expected<ProceduralPrefab> deskFromJson(const Json& params) {
    DeskParams p;
    ParamReader r("desk", params, {"width", "depth", "height", "computer"});
    r.read("width", p.width);
    r.read("depth", p.depth);
    r.read("height", p.height);
    r.read("computer", p.computer);
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildDesk(p);
}

Expected<ProceduralPrefab> officeChairFromJson(const Json& params) {
    OfficeChairParams p;
    ParamReader r("office_chair", params, {"width", "depth", "height", "seatHeight", "arms"});
    r.read("width", p.width);
    r.read("depth", p.depth);
    r.read("height", p.height);
    r.read("seatHeight", p.seatHeight);
    r.read("arms", p.arms);
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildOfficeChair(p);
}

Expected<ProceduralPrefab> barStoolFromJson(const Json& params) {
    BarStoolParams p;
    ParamReader r("bar_stool", params, {"height", "seatRadius", "baseRadius"});
    r.read("height", p.height);
    r.read("seatRadius", p.seatRadius);
    r.read("baseRadius", p.baseRadius);
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildBarStool(p);
}

Expected<ProceduralPrefab> bankerLampFromJson(const Json& params) {
    BankerLampParams p;
    ParamReader r("banker_lamp", params, {"standWidth", "standDepth", "standHeight", "shadeLength"});
    r.read("standWidth", p.standWidth);
    r.read("standDepth", p.standDepth);
    r.read("standHeight", p.standHeight);
    r.read("shadeLength", p.shadeLength);
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildBankerLamp(p);
}

Expected<ProceduralPrefab> showcaseFromJson(const Json& params) {
    ShowcaseParams p;
    ParamReader r("showcase", params, {"width", "height", "depth", "baseHeight", "item"});
    r.read("width", p.width);
    r.read("height", p.height);
    r.read("depth", p.depth);
    r.read("baseHeight", p.baseHeight);
    r.readEnum("item", p.item,
               {{"none", ShowcaseItem::None}, {"console", ShowcaseItem::Console}, {"handheld", ShowcaseItem::Handheld}});
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildShowcase(p);
}

Expected<ProceduralPrefab> displayCaseFromJson(const Json& params) {
    DisplayCaseParams p;
    ParamReader r("display_case", params,
                  {"width", "height", "depth", "baseHeight", "levels", "tiltDeg", "levelRise", "slotSize", "slotGap",
                   "slotMargin"});
    r.read("width", p.width);
    r.read("height", p.height);
    r.read("depth", p.depth);
    r.read("baseHeight", p.baseHeight);
    r.read("levels", p.levels);
    r.read("tiltDeg", p.tiltDeg);
    r.read("levelRise", p.levelRise);
    r.read("slotSize", p.slotSize);
    r.read("slotGap", p.slotGap);
    r.read("slotMargin", p.slotMargin);
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildDisplayCase(p);
}

Expected<ProceduralPrefab> wallShelfFromJson(const Json& params) {
    WallShelfParams p;
    ParamReader r(
        "wall_shelf", params,
        {"width", "height", "depth", "mountHeight", "ledges", "tiltDeg", "slotSize", "slotGap", "slotMargin",
         "boardLight"});
    r.read("width", p.width);
    r.read("height", p.height);
    r.read("depth", p.depth);
    r.read("mountHeight", p.mountHeight);
    r.read("ledges", p.ledges);
    r.read("tiltDeg", p.tiltDeg);
    r.read("slotSize", p.slotSize);
    r.read("slotGap", p.slotGap);
    r.read("slotMargin", p.slotMargin);
    r.read("boardLight", p.boardLight);
    if (!r.ok()) {
        return propagate(r.error());
    }
    return buildWallShelf(p);
}

/// The three shelf dressing kinds share their parameters.
template <Expected<ProceduralPrefab> (*Build)(const ShelfDressingParams&)>
Expected<ProceduralPrefab> dressingFromJson(std::string_view kind, const Json& params) {
    ShelfDressingParams p;
    ParamReader r(std::string(kind), params, {"width", "height", "depth", "seed"});
    r.read("width", p.width);
    r.read("height", p.height);
    r.read("depth", p.depth);
    r.read("seed", p.seed);
    if (!r.ok()) {
        return propagate(r.error());
    }
    return Build(p);
}

Expected<ProceduralPrefab> storageBoxFromJson(const Json& params) {
    return dressingFromJson<&buildStorageBox>("storage_box", params);
}

Expected<ProceduralPrefab> magazineStackFromJson(const Json& params) {
    return dressingFromJson<&buildMagazineStack>("magazine_stack", params);
}

Expected<ProceduralPrefab> bookendsFromJson(const Json& params) {
    return dressingFromJson<&buildBookends>("bookends", params);
}

// --- Primitives ----------------------------------------------------------------------------------------------

constexpr float kMinLength = 0.001f;
constexpr float kMaxLength = 1000.0f;
constexpr int kMaxSegments = 256;
constexpr int kMaxRings = 128;

Expected<MeshData> boxFromJson(const Json& params) {
    glm::vec3 size(1.0f);
    BoxUv uv = BoxUv::PerFace;
    float metersPerTile = 1.0f;
    glm::vec3 uvOrigin(0.0f);
    ParamReader r("box", params, {"size", "uv", "metersPerTile", "uvOrigin"});
    r.read("size", size);
    r.readEnum("uv", uv, {{"face", BoxUv::PerFace}, {"meters", BoxUv::WorldMeters}, {"grain", BoxUv::GrainMeters}});
    r.read("metersPerTile", metersPerTile);
    r.read("uvOrigin", uvOrigin);
    if (!r.ok()) {
        return propagate(r.error());
    }
    if (Status status = firstError({
            checkRange("box", "size", size, glm::vec3(kMinLength), glm::vec3(kMaxLength)),
            checkRange("box", "metersPerTile", metersPerTile, 0.01f, 100.0f),
            checkRange("box", "uvOrigin", uvOrigin, glm::vec3(-kMaxLength), glm::vec3(kMaxLength)),
        });
        !status) {
        return propagate(status.error());
    }
    MeshBuilder builder;
    if (uvOrigin == glm::vec3(0.0f)) {
        builder.box(size, glm::mat4(1.0f), uv, metersPerTile);
        return std::move(builder).build("box");
    }
    // The meter layouts measured from `uvOrigin` (the box's place in the scene): boxes side by side tile on.
    builder.box(size, detail::translation(uvOrigin), uv, metersPerTile);
    MeshData mesh = std::move(builder).build("box");
    for (Vertex& vertex : mesh.vertices) {
        vertex.position -= uvOrigin;
    }
    computeBounds(mesh);
    return mesh;
}

Expected<MeshData> planeFromJson(const Json& params) {
    glm::vec2 size(1.0f);
    glm::vec2 uvTiling(1.0f);
    ParamReader r("plane", params, {"size", "uvTiling"});
    r.read("size", size);
    r.read("uvTiling", uvTiling);
    if (!r.ok()) {
        return propagate(r.error());
    }
    if (Status status = firstError({
            checkRange("plane", "size", size, glm::vec2(kMinLength), glm::vec2(kMaxLength)),
            checkRange("plane", "uvTiling", uvTiling, glm::vec2(0.001f), glm::vec2(10000.0f)),
        });
        !status) {
        return propagate(status.error());
    }
    MeshBuilder builder;
    builder.plane(size, glm::mat4(1.0f), uvTiling);
    return std::move(builder).build("plane");
}

Expected<MeshData> quadFromJson(const Json& params) {
    glm::vec2 size(1.0f);
    ParamReader r("quad", params, {"size"});
    r.read("size", size);
    if (!r.ok()) {
        return propagate(r.error());
    }
    if (Status status = checkRange("quad", "size", size, glm::vec2(kMinLength), glm::vec2(kMaxLength)); !status) {
        return propagate(status.error());
    }
    MeshBuilder builder;
    builder.quad(size);
    return std::move(builder).build("quad");
}

Expected<MeshData> sphereFromJson(const Json& params) {
    float radius = 0.5f;
    int segments = 32;
    int rings = 16;
    ParamReader r("sphere", params, {"radius", "segments", "rings"});
    r.read("radius", radius);
    r.read("segments", segments);
    r.read("rings", rings);
    if (!r.ok()) {
        return propagate(r.error());
    }
    if (Status status = firstError({
            checkRange("sphere", "radius", radius, kMinLength, kMaxLength),
            checkRange("sphere", "segments", segments, 3, kMaxSegments),
            checkRange("sphere", "rings", rings, 2, kMaxRings),
        });
        !status) {
        return propagate(status.error());
    }
    MeshBuilder builder;
    builder.uvSphere(radius, segments, rings);
    return std::move(builder).build("sphere");
}

Expected<MeshData> cylinderFromJson(const Json& params) {
    float radius = 0.5f;
    float height = 1.0f;
    int segments = 32;
    bool capped = true;
    ParamReader r("cylinder", params, {"radius", "height", "segments", "capped"});
    r.read("radius", radius);
    r.read("height", height);
    r.read("segments", segments);
    r.read("capped", capped);
    if (!r.ok()) {
        return propagate(r.error());
    }
    if (Status status = firstError({
            checkRange("cylinder", "radius", radius, kMinLength, kMaxLength),
            checkRange("cylinder", "height", height, kMinLength, kMaxLength),
            checkRange("cylinder", "segments", segments, 3, kMaxSegments),
        });
        !status) {
        return propagate(status.error());
    }
    MeshBuilder builder;
    builder.cylinder(radius, height, segments, capped);
    return std::move(builder).build("cylinder");
}

// --- Registries ----------------------------------------------------------------------------------------------

struct PrefabEntry {
    std::string_view kind;
    Expected<ProceduralPrefab> (*build)(const Json&);
};

struct PrimitiveEntry {
    std::string_view kind;
    Expected<MeshData> (*build)(const Json&);
};

constexpr std::array<PrefabEntry, 23> kPrefabs{{
    {"room_shell", &roomShellFromJson},
    {"shelf", &shelfFromJson},
    {"cartridge", &cartridgeFromJson},
    {"crt", &crtFromJson},
    {"tv_cabinet", &tvCabinetFromJson},
    {"console_cartridge", &consoleCartridgeFromJson},
    {"poster_frame", &posterFrameFromJson},
    {"rug", &rugFromJson},
    {"floor_lamp", &floorLampFromJson},
    {"armchair", &armchairFromJson},
    {"display_case", &displayCaseFromJson},
    {"wall_shelf", &wallShelfFromJson},
    {"storage_box", &storageBoxFromJson},
    {"magazine_stack", &magazineStackFromJson},
    {"bookends", &bookendsFromJson},
    {"plant", &plantFromJson},
    {"sofa", &sofaFromJson},
    {"beanbag", &beanbagFromJson},
    {"desk", &deskFromJson},
    {"office_chair", &officeChairFromJson},
    {"bar_stool", &barStoolFromJson},
    {"banker_lamp", &bankerLampFromJson},
    {"showcase", &showcaseFromJson},
}};

constexpr std::array<PrimitiveEntry, 5> kPrimitives{{
    {"box", &boxFromJson},
    {"plane", &planeFromJson},
    {"quad", &quadFromJson},
    {"sphere", &sphereFromJson},
    {"cylinder", &cylinderFromJson},
}};

template <class Entries>
constexpr auto kindNames(const Entries& entries) {
    std::array<std::string_view, std::tuple_size_v<Entries>> names{};
    for (std::size_t i = 0; i < names.size(); ++i) {
        names[i] = entries[i].kind;
    }
    return names;
}

constexpr auto kPrefabKinds = kindNames(kPrefabs);
constexpr auto kPrimitiveKinds = kindNames(kPrimitives);

std::string joinKinds(std::span<const std::string_view> kinds) {
    std::string text;
    for (const std::string_view kind : kinds) {
        text += text.empty() ? "" : ", ";
        text += kind;
    }
    return text;
}

} // namespace

const PrefabPart* findPart(const ProceduralPrefab& prefab, std::string_view role) noexcept {
    for (const PrefabPart& part : prefab.parts) {
        if (part.materialRole == role) {
            return &part;
        }
    }
    return nullptr;
}

Expected<ProceduralPrefab> buildPrefab(std::string_view kind, const nlohmann::json& params) {
    for (const PrefabEntry& entry : kPrefabs) {
        if (entry.kind == kind) {
            return entry.build(params);
        }
    }
    return makeError(ErrorCode::NotFound, "unknown prefab kind (expected one of: " + joinKinds(kPrefabKinds) + ")",
                     std::string(kind));
}

Expected<MeshData> buildPrimitive(std::string_view kind, const nlohmann::json& params) {
    for (const PrimitiveEntry& entry : kPrimitives) {
        if (entry.kind == kind) {
            return entry.build(params);
        }
    }
    return makeError(ErrorCode::NotFound,
                     "unknown primitive kind (expected one of: " + joinKinds(kPrimitiveKinds) + ")", std::string(kind));
}

std::span<const std::string_view> prefabKinds() noexcept {
    return kPrefabKinds;
}

std::span<const std::string_view> primitiveKinds() noexcept {
    return kPrimitiveKinds;
}

} // namespace rv::geometry
