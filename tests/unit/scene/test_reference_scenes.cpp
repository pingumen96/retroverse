#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/aabb.hpp>
#include <rv/core/json.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/scene/batch_planner.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/collision.hpp>
#include <rv/scene/scene_description.hpp>

using Catch::Approx;
using nlohmann::json;
using rv::scene::LightType;
using rv::scene::SceneDescription;
using rv::scene::SceneObject;
using rv::scene::TextureSource;

namespace {

// --- Shared contracts (they must match the material library and the procedural geometry EXACTLY) ------------

const std::set<std::string> kSharedMaterials{
    "MAT_Wood_Walnut",     "MAT_Wood_Oak",          "MAT_Wood_Varnished", "MAT_Floor_OakPlanks", "MAT_Concrete_Warm",
    "MAT_Wall_Painted",    "MAT_Plaster_Used",      "MAT_Metal_Graphite", "MAT_Metal_Brushed",   "MAT_Plastic_Black",
    "MAT_Plastic_Gray",    "MAT_Plastic_AgedIvory", "MAT_Plastic_Accent", "MAT_Glass_Display",   "MAT_Glass_CRT",
    "MAT_Fabric_Sofa",     "MAT_Carpet_Ornate",     "MAT_Cardboard_Box",  "MAT_Paper_Print",     "MAT_Neon_Diffuser",
    "MAT_Screen_Emissive", "MAT_Screen_CRT",        "MAT_Lamp_Shade",     "MAT_Label_Print",     "MAT_Fallback_Magenta",
};

/// Material roles of every procedural prefab.
const std::map<std::string, std::set<std::string>> kPrefabRoles{
    {"room_shell", {"floor", "wall", "ceiling", "trim", "wainscot"}},
    {"shelf", {"wood_primary", "wood_secondary"}},
    {"cartridge", {"plastic_shell", "label"}},
    {"crt", {"plastic_body", "plastic_bezel", "screen", "plastic_dark"}},
    {"tv_cabinet", {"wood_primary", "wood_secondary", "metal_trim"}},
    {"console_cartridge", {"plastic_body", "plastic_dark", "plastic_accent"}},
    {"poster_frame", {"frame", "picture"}},
    {"rug", {"fabric"}},
    {"floor_lamp", {"metal", "lamp_shade"}},
    {"armchair", {"fabric", "wood"}},
};

/// Parameter keys of every prefab and primitive.
const std::map<std::string, std::set<std::string>> kParamKeys{
    {"room_shell",
     {"size", "wallHeight", "wallThickness", "metersPerTile", "ceiling", "baseboardHeight", "wainscotHeight",
      "doorway"}},
    {"shelf",
     {"width", "height", "depth", "levels", "boardThickness", "back", "plinthHeight", "slotSize", "slotGap",
      "slotMargin"}},
    {"cartridge", {"size", "chamfer", "labelMargin", "labelInset", "gripRidges"}},
    {"crt", {"width", "height", "depth", "screenAspect", "bezel", "screenInset", "backTaper"}},
    {"tv_cabinet", {"width", "height", "depth", "drawers", "legHeight"}},
    {"console_cartridge", {"width", "height", "depth"}},
    {"poster_frame", {"width", "height", "frameWidth", "depth"}},
    {"rug", {"width", "depth", "thickness"}},
    {"floor_lamp", {"height", "baseRadius", "shadeRadius", "shadeHeight"}},
    {"armchair", {"width", "depth", "height", "seatHeight", "armHeight", "legHeight"}},
    {"box", {"size", "uv", "metersPerTile"}},
    {"plane", {"size", "uvTiling"}},
    {"quad", {"size"}},
    {"sphere", {"radius", "segments", "rings"}},
    {"cylinder", {"radius", "height", "segments", "capped"}},
};

const glm::vec3 kDefaultCartridgeSize(0.125f, 0.085f, 0.020f);

/// Back-panel thickness of rv_geometry's shelf builder. The shared contract centers slot items "in depth on the
/// board" but leaves the back panel open; the boards start in front of it.
constexpr float kShelfBackPanelThickness = 0.008f;

/// Shelf slot layout from the shared parameter contract (defaults for missing keys). It replicates the slot formula
/// of the contract on purpose: tests/unit/integration/test_reference_scenes_build.cpp compares the cartridges with the
/// Slot_* sockets of the built shelves.
struct ShelfLayout {
    float width = 1.2f;
    float height = 2.0f;
    int levels = 5;
    float boardThickness = 0.025f;
    bool back = true;
    float plinthHeight = 0.08f;
    glm::vec3 slotSize = kDefaultCartridgeSize;
    float slotGap = 0.004f;
    float slotMargin = 0.03f;

    static ShelfLayout fromParams(const json& params) {
        ShelfLayout layout;
        layout.width = params.value("width", layout.width);
        layout.height = params.value("height", layout.height);
        layout.levels = params.value("levels", layout.levels);
        layout.boardThickness = params.value("boardThickness", layout.boardThickness);
        layout.back = params.value("back", layout.back);
        layout.plinthHeight = params.value("plinthHeight", layout.plinthHeight);
        if (params.contains("slotSize")) {
            layout.slotSize = rv::core::jsonToVec3(params.at("slotSize"), "slotSize").value();
        }
        layout.slotGap = params.value("slotGap", layout.slotGap);
        layout.slotMargin = params.value("slotMargin", layout.slotMargin);
        return layout;
    }

    [[nodiscard]] float innerWidth() const { return width - 2.0f * boardThickness; }

    [[nodiscard]] int slotsPerLevel() const {
        return static_cast<int>(std::floor((innerWidth() - 2.0f * slotMargin + slotGap) / (slotSize.x + slotGap)));
    }

    /// Socket translation of Slot_<level>_<index>: bottom-center of the item on the board, in shelf space.
    [[nodiscard]] glm::vec3 slotBottomCenter(int level, int index) const {
        const float x =
            -0.5f * innerWidth() + slotMargin + 0.5f * slotSize.x + static_cast<float>(index) * (slotSize.x + slotGap);
        const float pitch = (height - boardThickness - plinthHeight) / static_cast<float>(levels);
        const float y = plinthHeight + boardThickness + static_cast<float>(level) * pitch;
        const float z = back ? 0.5f * kShelfBackPanelThickness : 0.0f;
        return {x, y, z};
    }
};

std::filesystem::path referenceScenePath(const char* name) {
    return rv::core::pathFromUtf8(RV_TEST_ASSET_DIR "/style_tests/reference_scenes/") / (std::string(name) + ".json");
}

SceneDescription loadReference(const char* name) {
    auto scene = rv::scene::loadSceneDescription(referenceScenePath(name));
    INFO((scene ? std::string() : scene.error().toString()));
    REQUIRE(scene.has_value());
    return std::move(*scene);
}

std::vector<const SceneObject*> objectsOfKind(const SceneDescription& scene, std::string_view kind) {
    std::vector<const SceneObject*> result;
    for (const SceneObject& object : scene.objects) {
        if (object.source != SceneObject::Source::Model && object.kind == kind) {
            result.push_back(&object);
        }
    }
    return result;
}

glm::vec3 translationOf(const glm::mat4& transform) {
    return glm::vec3(transform[3]);
}

bool isPrimitive(const SceneObject& object) {
    return object.source == SceneObject::Source::Primitive;
}

/// Checks kinds, parameter keys, roles and material names of every object against the shared contracts.
void checkSharedContracts(const SceneDescription& scene) {
    for (const SceneObject& object : scene.objects) {
        INFO("object " << object.id);
        REQUIRE(object.source != SceneObject::Source::Model); // reference scenes are fully procedural
        const auto keys = kParamKeys.find(object.kind);
        REQUIRE(keys != kParamKeys.end());
        CHECK(isPrimitive(object) == !kPrefabRoles.contains(object.kind));
        for (const auto& param : object.params.items()) {
            INFO("param " << param.key());
            CHECK(keys->second.contains(param.key()));
        }

        if (!object.material.empty()) {
            CHECK(kSharedMaterials.contains(object.material));
        }
        for (const auto& entry : object.materialsByRole) {
            INFO("role " << entry.first);
            CHECK(kSharedMaterials.contains(entry.second));
        }

        if (isPrimitive(object)) {
            // One material per primitive: a library name or an inline definition.
            CHECK(object.materialsByRole.empty());
            CHECK(object.texturesByRole.empty());
            CHECK(object.material.empty() != object.inlineMaterial.is_null());
            continue;
        }
        const std::set<std::string>& roles = kPrefabRoles.at(object.kind);
        for (const auto& entry : object.materialsByRole) {
            INFO("role " << entry.first);
            CHECK(roles.contains(entry.first));
        }
        for (const auto& entry : object.texturesByRole) {
            INFO("texture role " << entry.first);
            CHECK(roles.contains(entry.first));
        }
        for (const std::string& role : roles) {
            INFO("role " << role);
            CHECK_FALSE(object.materialNameForRole(role).empty()); // every part gets a library material
        }
    }
}

/// Camera of a scene setup as the application builds it (16:9 viewport).
rv::scene::Camera cameraOf(const rv::scene::CameraSetup& setup) {
    rv::scene::Camera camera;
    camera.setLens(setup.lens);
    camera.setAspect(16.0f / 9.0f);
    camera.setPose(setup.position, rv::core::degreesToRadians(setup.yawDeg),
                   rv::core::degreesToRadians(setup.pitchDeg));
    return camera;
}

struct Footprint {
    glm::vec2 min;
    glm::vec2 max;
    float height;
};

/// XZ footprint of floor-standing furniture from its contract parameters (unrotated objects only).
Footprint footprintOf(const SceneObject& object) {
    const glm::vec3 position = translationOf(object.transform);
    glm::vec2 half(0.0f);
    float height = 0.0f;
    if (object.kind == "floor_lamp") {
        const float radius =
            std::max(object.params.value("baseRadius", 0.16f), object.params.value("shadeRadius", 0.22f));
        half = glm::vec2(radius);
        height = object.params.value("height", 1.6f);
    } else {
        half = glm::vec2(0.5f * object.params.at("width").get<float>(), 0.5f * object.params.at("depth").get<float>());
        height = object.params.at("height").get<float>();
    }
    return {glm::vec2(position.x, position.z) - half, glm::vec2(position.x, position.z) + half, height};
}

float gapBetween(const Footprint& a, const Footprint& b) {
    const float dx = std::max({0.0f, b.min.x - a.max.x, a.min.x - b.max.x});
    const float dz = std::max({0.0f, b.min.y - a.max.y, a.min.y - b.max.y});
    return std::sqrt(dx * dx + dz * dz);
}

/// Cone factor of `light` at `point`, as the lit shader computes it (common/lights.glsl): 1 inside the inner cone of a
/// spot, 0 outside its outer cone, a smoothstep between them; 1 for other lights.
float coneFactor(const rv::scene::Light& light, const glm::vec3& point) {
    if (light.type != LightType::Spot) {
        return 1.0f;
    }
    const float cosOuter = std::cos(rv::core::degreesToRadians(light.outerConeDeg));
    const float cosInner = std::max(std::cos(rv::core::degreesToRadians(light.innerConeDeg)), cosOuter);
    const float cosAngle = glm::dot(glm::normalize(light.direction), glm::normalize(point - light.position));
    const float t = std::clamp((cosAngle - cosOuter) / std::max(cosInner - cosOuter, 1e-4f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// Irradiance that a point or spot `light` brings to a surface at `point` facing the unit `normal`, as the lit shader
/// computes it (common/lights.glsl): the intensity, the inverse-square falloff with its window reaching 0 at the range,
/// the cone factor and the cosine of incidence. The light's color is left out.
float irradiance(const rv::scene::Light& light, const glm::vec3& point, const glm::vec3& normal) {
    const glm::vec3 toLight = light.position - point;
    const float distanceSquared = glm::dot(toLight, toLight);
    const float rangeSquared = light.range * light.range;
    if (distanceSquared >= rangeSquared || distanceSquared <= 0.0f) {
        return 0.0f;
    }
    const float ratio = distanceSquared / rangeSquared;
    const float window = std::clamp(1.0f - ratio * ratio, 0.0f, 1.0f);
    const float incidence = std::max(glm::dot(normal, toLight / std::sqrt(distanceSquared)), 0.0f);
    return light.intensity * window * window / distanceSquared * coneFactor(light, point) * incidence;
}

} // namespace

TEST_CASE("scene/reference_scenes: the reference scenes load", "[scene][reference_scenes]") {
    for (const char* name : {"m0_testroom", "material_test", "instancing_stress", "instancing_growth"}) {
        INFO("scene " << name);
        const SceneDescription scene = loadReference(name);
        CHECK(scene.name == name);
        CHECK_FALSE(scene.objects.empty());
        CHECK_FALSE(scene.lights.empty());
        checkSharedContracts(scene);
    }
}

TEST_CASE("scene/reference_scenes: instancing_stress expands to exactly 1000 boxes in 3 batches",
          "[scene][reference_scenes]") {
    const SceneDescription scene = loadReference("instancing_stress");
    const auto boxes = objectsOfKind(scene, "box");
    REQUIRE(boxes.size() == 3);

    std::size_t boxInstances = 0;
    std::set<std::string> materials;
    std::vector<std::size_t> counts;
    for (const SceneObject* box : boxes) {
        CHECK(box->params == boxes[0]->params); // identical parameters: one shared mesh
        CHECK_FALSE(box->instances.empty());
        boxInstances += box->instanceTransforms().size();
        counts.push_back(box->instanceTransforms().size());
        materials.insert(box->material);
    }
    CHECK(boxInstances == 1000);
    CHECK(counts == std::vector<std::size_t>{400, 300, 300});
    CHECK(materials.size() == 3);

    // Feed the scene to the batch planner the way the application does: one mesh per (kind, params), one
    // material per name. Boxes give 3 batches of 400 + 300 + 300, the floor a fourth one.
    std::map<std::string, std::uint32_t> meshIds;
    std::map<std::string, std::uint32_t> materialIds;
    std::vector<rv::scene::RenderItem> items;
    for (const SceneObject& object : scene.objects) {
        const auto mesh =
            meshIds.emplace(object.kind + object.params.dump(), static_cast<std::uint32_t>(meshIds.size()));
        const auto material = materialIds.emplace(object.material, static_cast<std::uint32_t>(materialIds.size()));
        for (const glm::mat4& model : object.instanceTransforms()) {
            items.push_back({rv::scene::MeshHandle{mesh.first->second},
                             rv::scene::MaterialHandle{material.first->second}, rv::scene::ShaderKind::Pbr, model,
                             object.castsShadows});
        }
    }
    const rv::scene::BatchPlan plan = rv::scene::BatchPlanner{}.plan(items);
    CHECK(plan.instanceTransforms.size() == 1001);
    REQUIRE(plan.batches.size() == 4);
    std::multiset<std::uint32_t> batchSizes;
    for (const auto& batch : plan.batches) {
        batchSizes.insert(batch.instanceCount);
    }
    CHECK(batchSizes == std::multiset<std::uint32_t>{1, 300, 300, 400});

    // The sun casts the shadows (the reference of the directional shadow map): the shadow pass draws the boxes in 3
    // batches, without the floor.
    REQUIRE(scene.lights.size() == 1);
    CHECK(scene.lights[0].type == LightType::Directional);
    CHECK(scene.lights[0].castsShadows);
    rv::scene::PlanOptions casters;
    casters.shadowCastersOnly = true;
    const rv::scene::BatchPlan shadow = rv::scene::BatchPlanner{}.plan(items, casters);
    CHECK(shadow.instanceTransforms.size() == 1000);
    CHECK(shadow.batches.size() == 3);

    // The camera frames every box.
    const rv::scene::Frustum frustum = cameraOf(scene.camera).frustum();
    for (const SceneObject* box : boxes) {
        for (const glm::mat4& model : box->instanceTransforms()) {
            CHECK(frustum.contains(translationOf(model)));
        }
    }
}

TEST_CASE("scene/reference_scenes: material_test sweeps roughness along X and metallic along Y",
          "[scene][reference_scenes]") {
    const SceneDescription scene = loadReference("material_test");
    const auto spheres = objectsOfKind(scene, "sphere");
    REQUIRE(spheres.size() == 25);
    CHECK(objectsOfKind(scene, "plane").size() == 1);
    CHECK(scene.lights.size() == 2);

    std::map<float, std::set<float>> roughnessByX;
    std::map<float, std::set<float>> metallicByY;
    const rv::scene::Frustum frustum = cameraOf(scene.camera).frustum();
    for (const SceneObject* sphere : spheres) {
        REQUIRE(sphere->inlineMaterial.is_object());
        CHECK(sphere->params.value("radius", 0.5f) == 0.35f);
        const glm::vec3 center = translationOf(sphere->transform);
        roughnessByX[center.x].insert(sphere->inlineMaterial.at("roughness").get<float>());
        metallicByY[center.y].insert(sphere->inlineMaterial.at("metallic").get<float>());
        CHECK(frustum.contains(center));
    }
    // Five columns and five rows, each with a single value, increasing along the axis.
    REQUIRE(roughnessByX.size() == 5);
    REQUIRE(metallicByY.size() == 5);
    float previous = -1.0f;
    for (const auto& column : roughnessByX) {
        REQUIRE(column.second.size() == 1);
        CHECK(*column.second.begin() > previous);
        previous = *column.second.begin();
    }
    CHECK(*roughnessByX.begin()->second.begin() == Approx(0.05f));
    CHECK(previous == Approx(1.0f));
    previous = -1.0f;
    for (const auto& row : metallicByY) {
        REQUIRE(row.second.size() == 1);
        CHECK(*row.second.begin() > previous);
        previous = *row.second.begin();
    }
    CHECK(*metallicByY.begin()->second.begin() == Approx(0.0f));
    CHECK(previous == Approx(1.0f));
}

TEST_CASE("scene/reference_scenes: m0_testroom composition", "[scene][reference_scenes][m0_testroom]") {
    const SceneDescription scene = loadReference("m0_testroom");

    std::set<std::string> ids;
    for (const SceneObject& object : scene.objects) {
        CHECK(ids.insert(object.id).second);
    }

    // Room: 9 x 10 m, 3 m high, doorway in the south wall. Oak planks and painted plaster repeat every 2 m (the repeat
    // of their procedural surfaces); a varnished wainscot under a rail lines the walls.
    const auto rooms = objectsOfKind(scene, "room_shell");
    REQUIRE(rooms.size() == 1);
    const json& room = rooms[0]->params;
    CHECK(room.at("size") == json::array({9.0, 10.0}));
    CHECK(room.at("wallHeight").get<float>() == 3.0f);
    CHECK(room.at("doorway").at("wall") == "south");
    CHECK(room.at("metersPerTile").get<float>() == 2.0f);
    CHECK(room.at("wainscotHeight").get<float>() == Approx(1.0f).margin(0.2f));
    CHECK(rooms[0]->materialNameForRole("floor") == "MAT_Floor_OakPlanks");
    CHECK(rooms[0]->materialNameForRole("wall") == "MAT_Wall_Painted");
    CHECK(rooms[0]->materialNameForRole("ceiling") == "MAT_Wall_Painted"); // dark and matte: no glare
    CHECK(rooms[0]->materialNameForRole("trim") == "MAT_Wood_Walnut");
    CHECK(rooms[0]->materialNameForRole("wainscot") == "MAT_Wood_Varnished");

    // The camera stands at eye height inside the room, looking towards the north wall, the collection and hero wall,
    // a little down.
    CHECK(scene.camera.position.y == Approx(1.65f));
    CHECK(std::abs(scene.camera.position.x) < 4.5f - 0.3f);
    CHECK(std::abs(scene.camera.position.z) < 5.0f - 0.3f);
    CHECK(std::abs(scene.camera.yawDeg) <= 20.0f);
    CHECK(scene.camera.pitchDeg <= 0.0f);
    CHECK(scene.camera.pitchDeg >= -15.0f);

    // It frames the collection, the sign, the TV corner with the rug and a practical lamp (style 1.1: a game room,
    // collectible objects and a lit screen, read within five seconds).
    const rv::scene::Frustum frustum = cameraOf(scene.camera).frustum();
    const auto shelves = objectsOfKind(scene, "shelf");
    const auto crts = objectsOfKind(scene, "crt");
    const auto lamps = objectsOfKind(scene, "floor_lamp");
    REQUIRE(shelves.size() == 2);
    REQUIRE(crts.size() == 1);
    REQUIRE(lamps.size() == 2);                         // the floor lamp and the table lamp
    REQUIRE(objectsOfKind(scene, "tv_cabinet").size() == 2); // the TV cabinet and the sideboard
    const SceneObject* shelf = scene.findObject("shelf_main");
    const SceneObject* sign = scene.findObject("room_sign");
    const SceneObject* rug = scene.findObject("rug");
    const SceneObject* tableLamp = scene.findObject("table_lamp");
    const SceneObject* cabinet = scene.findObject("tv_cabinet");
    REQUIRE(shelf != nullptr);
    REQUIRE(sign != nullptr);
    REQUIRE(rug != nullptr);
    REQUIRE(tableLamp != nullptr);
    REQUIRE(cabinet != nullptr);
    const glm::vec3 screen = translationOf(crts[0]->transform) + glm::vec3(0.0f, 0.3f, 0.0f);
    for (const SceneObject* collection : shelves) {
        INFO(collection->id);
        CHECK(frustum.contains(translationOf(collection->transform) + glm::vec3(0.0f, 1.0f, 0.0f)));
    }
    CHECK(frustum.contains(screen));
    CHECK(frustum.contains(translationOf(sign->transform)));
    CHECK(frustum.contains(translationOf(rug->transform)));
    CHECK(frustum.contains(translationOf(tableLamp->transform) + glm::vec3(0.0f, 0.5f, 0.0f)));

    // Both shelves against the north wall, shelf_main at x = -1.5 (tests/input_scripts/walk_into_shelf.json); the TV
    // cabinet at x = +1.8 with the CRT on top.
    for (const SceneObject* collection : shelves) {
        INFO(collection->id);
        CHECK(translationOf(collection->transform).z - 0.5f * collection->params.at("depth").get<float>() > -5.0f);
        CHECK(translationOf(collection->transform).z < -4.5f);
        CHECK(collection->materialNameForRole("wood_primary") == "MAT_Wood_Walnut");
        CHECK(collection->materialNameForRole("wood_secondary") == "MAT_Wood_Varnished");
    }
    CHECK(translationOf(shelf->transform).x == Approx(-1.5f));
    CHECK(translationOf(cabinet->transform).x == Approx(1.8f));
    CHECK(translationOf(crts[0]->transform).y == Approx(cabinet->params.at("height").get<float>()));
    // The CRT rests on the cabinet top entirely: no overhang at the front (0.1 mm tolerance for float rounding).
    const Footprint cabinetTop = footprintOf(*cabinet);
    const Footprint crtBase = footprintOf(*crts[0]);
    CHECK(crtBase.min.x >= cabinetTop.min.x - 1e-4f);
    CHECK(crtBase.max.x <= cabinetTop.max.x + 1e-4f);
    CHECK(crtBase.min.y >= cabinetTop.min.y - 1e-4f);
    CHECK(crtBase.max.y <= cabinetTop.max.y + 1e-4f);
    // The picture tube shows the color bars (style 15.2, CRT_Subtle of style 25).
    CHECK(crts[0]->materialNameForRole("screen") == "MAT_Screen_CRT");
    CHECK(crts[0]->texturesByRole.at("screen").pattern == "color_bars");
    CHECK(objectsOfKind(scene, "console_cartridge").size() == 1);
    // The rug lies in front of the TV, under it in x.
    REQUIRE(objectsOfKind(scene, "rug").size() == 1);
    const glm::vec2 rugCenter(translationOf(rug->transform).x, translationOf(rug->transform).z);
    const glm::vec2 rugHalf(0.5f * rug->params.at("width").get<float>(), 0.5f * rug->params.at("depth").get<float>());
    const Footprint rugArea{rugCenter - rugHalf, rugCenter + rugHalf, 0.0f};
    CHECK(rugArea.min.x < screen.x);
    CHECK(rugArea.max.x > screen.x);
    CHECK(rugArea.max.y < scene.camera.position.z);
    CHECK(rugArea.min.y > cabinetTop.max.y - 0.5f);
    // The armchair is turned towards the TV: its front (+Z) points at the screen within 15 degrees.
    const auto chairs = objectsOfKind(scene, "armchair");
    REQUIRE(chairs.size() == 1);
    const glm::vec3 chairFront = glm::normalize(glm::vec3(chairs[0]->transform * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)));
    const glm::vec3 toScreen = screen - translationOf(chairs[0]->transform);
    CHECK(glm::dot(chairFront, glm::normalize(glm::vec3(toScreen.x, 0.0f, toScreen.z))) >
          std::cos(rv::core::degreesToRadians(15.0f)));
    CHECK(chairs[0]->materialNameForRole("fabric") == "MAT_Fabric_Sofa");
    // Five framed pictures: four posters and the room's sign, the lettering of an original theme name (style 17.2) in
    // an unlit material, the 10% accent of style 3.1.
    CHECK(objectsOfKind(scene, "poster_frame").size() == 5);
    CHECK(sign->texturesByRole.at("picture").pattern == "sign");
    CHECK(sign->texturesByRole.at("picture").title == "16-BIT CORNER");
    CHECK(sign->materialNameForRole("picture") == "MAT_Neon_Diffuser");

    // Everything collides except the rug and the framed pictures, which cast no shadow either: a flat rug would only
    // shadow the floor under it, and the frames are thin geometry (2 to 4 cm deep, they would cast shadows narrower
    // than the shadow filter, broken into dashes by the texels of the map).
    for (const SceneObject& object : scene.objects) {
        INFO("object " << object.id);
        const bool flat = object.kind == "rug" || object.kind == "poster_frame";
        CHECK(object.collides == !flat);
        CHECK(object.castsShadows == !flat);
        if (object.kind == "poster_frame" && &object != sign) {
            CHECK(object.texturesByRole.at("picture").pattern == "cover");
        }
    }

    // Lighting (style 3.1 and 4.2, SNES preset): warm tungsten mood with neutrals that stay neutral. The practical
    // lamps carry the amber tungsten, each with its bulb in its shade; the ceiling lights are a warm white, much less
    // saturated (an amber key turned every surface orange); a light magenta accent washes the TV corner and the sign
    // glows magenta onto its wall; the screen lights what lies in front of it; the entrance half has a light of its
    // own; nothing lights the dark ceiling from up close; the ambient is low, warm and near neutral, and darkened by
    // the ambient occlusion. Shadows (M0 step 11): exactly one light casts them, the key: a warm-white downlight in the
    // ceiling whose cone takes in the collection and the TV.
    const glm::vec3 tungsten = rv::core::hexToLinearRgb("#FFC88A").value();
    const glm::vec3 magenta = rv::core::hexToLinearRgb("#B34DCE").value();
    const float wallHeight = room.at("wallHeight").get<float>();
    // Linear chroma of a warm color: 1 - blue / red (0 for white, about 0.75 for tungsten).
    const auto warmth = [](const glm::vec3& color) { return 1.0f - color.b / color.r; };
    const auto inLamp = [](const rv::scene::Light& light, const SceneObject& lamp) {
        const glm::vec3 base = translationOf(lamp.transform);
        const float height = lamp.params.at("height").get<float>();
        const float shadeHeight = lamp.params.at("shadeHeight").get<float>();
        return std::abs(light.position.x - base.x) < 1e-4f && std::abs(light.position.z - base.z) < 1e-4f &&
               light.position.y > base.y + height - shadeHeight && light.position.y < base.y + height;
    };
    // The CRT's volume: the screen's light sits inside it, behind the glass.
    const glm::vec3 crtPosition = translationOf(crts[0]->transform);
    const glm::vec3 crtHalf(0.5f * crts[0]->params.at("width").get<float>(), 0.0f,
                            0.5f * crts[0]->params.at("depth").get<float>());
    const rv::core::Aabb crtVolume = rv::core::Aabb::fromMinMax(
        crtPosition - crtHalf,
        crtPosition + crtHalf + glm::vec3(0.0f, crts[0]->params.at("height").get<float>(), 0.0f));
    int accents = 0;
    int bulbs = 0;
    int lampLights = 0;
    int screenLights = 0;
    int entranceLights = 0;
    for (const rv::scene::Light& light : scene.lights) {
        INFO("light " << light.name);
        CHECK(light.position.y > 0.0f);
        CHECK(light.position.y < wallHeight);
        entranceLights += light.position.z > 0.0f ? 1 : 0;
        const bool practical =
            std::any_of(lamps.begin(), lamps.end(), [&](const SceneObject* lamp) { return inLamp(light, *lamp); });
        if (rv::core::nearlyEqual(light.color, magenta)) {
            ++accents;
            CHECK(light.intensity < 10.0f); // an accent, not a key light
            if (light.type == LightType::Spot) {
                const float cosAngle = glm::dot(light.direction, glm::normalize(screen - light.position));
                CHECK(cosAngle > std::cos(rv::core::degreesToRadians(light.innerConeDeg)));
            } else {
                CHECK(glm::length(light.position - translationOf(sign->transform)) < 0.5f); // the sign's glow
            }
        } else if (practical) {
            // The bulb inside the shade, and cones leaving through the shade's openings.
            ++lampLights;
            bulbs += light.type == LightType::Point ? 1 : 0;
            CHECK(rv::core::nearlyEqual(light.color, tungsten));
        } else if (crtVolume.contains(light.position)) {
            // The screen's light (style 4.3): from just behind the glass, so that the glass never mirrors it, forward
            // over the hemisphere in front of the screen (a spot: nothing behind the TV is lit), neutral like the
            // picture's average, dim.
            ++screenLights;
            CHECK(light.type == LightType::Spot);
            CHECK(light.position.z < crtVolume.max.z);
            CHECK(light.direction.z > 0.9f);
            CHECK(light.color.b >= light.color.r);
            CHECK(light.intensity < 5.0f);
        } else {
            CHECK(light.color.r >= light.color.g);
            CHECK(light.color.g >= light.color.b);
            CHECK(warmth(light.color) > 0.0f);
            CHECK(warmth(light.color) < 0.7f * warmth(tungsten));
            if (light.type == LightType::Spot) {
                // The cone stays below the horizontal (the axis points down at least the outer cone angle).
                CHECK(std::asin(-light.direction.y) >= rv::core::degreesToRadians(light.outerConeDeg) - 1e-4f);
            } else {
                CHECK(light.position.y <= wallHeight - 0.8f); // its glow on the ceiling stays soft
            }
        }
    }
    CHECK(accents == 2);
    CHECK(bulbs == 2); // one per lamp
    CHECK(lampLights >= 2);
    CHECK(screenLights == 1);
    CHECK(entranceLights >= 1);
    CHECK(scene.environment.ambientIntensity < 0.3f);
    const glm::vec3& sky = scene.environment.ambientSky;
    CHECK(warmth(sky) > 0.0f);
    CHECK(warmth(sky) < 0.2f);

    // The key light: the first light, and the only one, that casts shadows.
    REQUIRE_FALSE(scene.lights.empty());
    const rv::scene::Light& key = scene.lights.front();
    CHECK(key.castsShadows);
    CHECK(std::count_if(scene.lights.begin(), scene.lights.end(),
                        [](const rv::scene::Light& light) { return light.castsShadows; }) == 1);
    CHECK(key.type == LightType::Spot); // point lights cast no shadow
    CHECK(key.shadowStrength == 1.0f);
    CHECK(key.position.y > wallHeight - 0.1f); // in the ceiling
    // A downlight with a wide cone: pointing more down than towards the wall, the cone ending below the horizontal.
    const float tilt = std::acos(-key.direction.y);
    CHECK(tilt < rv::core::degreesToRadians(45.0f));
    CHECK(key.outerConeDeg >= 40.0f);
    CHECK(std::asin(-key.direction.y) >= rv::core::degreesToRadians(key.outerConeDeg) - 1e-4f);
    // Between the room's center and the hero wall; the main shelf and the TV well inside its cone.
    CHECK(key.position.z < 0.0f);
    CHECK(key.position.z > -3.5f);
    CHECK(coneFactor(key, translationOf(shelf->transform) + glm::vec3(0.0f, 1.0f, 0.0f)) > 0.75f);
    CHECK(coneFactor(key, screen) > 0.75f);
    // Warm white like the other room lights.
    CHECK(warmth(key.color) > 0.0f);
    CHECK(warmth(key.color) < 0.7f * warmth(tungsten));
    CHECK(key.range >= 8.0f);

    // The CRT is the focal point (style 6.1, 15): the key is aimed at the hero wall rather than at the floor, which
    // reflects more light than the walls (oak against painted plaster), so its brightest spot on the floor gets less
    // light than its brightest spot on the wall (interior x in [-4.5, 4.5], z in [-5, 5]; the hero wall at z = -5).
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 south(0.0f, 0.0f, 1.0f); // the hero wall faces the room
    float floorPeak = 0.0f;
    float wallPeak = 0.0f;
    for (int i = -44; i <= 44; ++i) {
        const float x = 0.1f * static_cast<float>(i);
        for (int j = -49; j <= 49; ++j) {
            floorPeak = std::max(floorPeak, irradiance(key, glm::vec3(x, 0.0f, 0.1f * static_cast<float>(j)), up));
        }
        for (int j = 1; j < 30; ++j) {
            wallPeak = std::max(wallPeak, irradiance(key, glm::vec3(x, 0.1f * static_cast<float>(j), -5.0f), south));
        }
    }
    REQUIRE(wallPeak > 0.0f);
    CHECK(floorPeak < 0.9f * wallPeak);
    // The room's center, beyond the key's cone, gets warm light from the ceiling too (the pendant), at least a fifth of
    // the key's brightest on the floor: the walk in from the doorway is not dark.
    float centerLight = 0.0f;
    for (const rv::scene::Light& light : scene.lights) {
        if (&light != &key && !rv::core::nearlyEqual(light.color, magenta)) {
            centerLight += irradiance(light, glm::vec3(0.0f), up);
        }
    }
    CHECK(centerLight > 0.2f * floorPeak);
    // The posters of the hero wall hang in warm light, and so does the wall between the TV and the right poster (style
    // 4.2: tungsten dominant, the magenta accents stay accents): the light reaching them is red over green over blue.
    // The right poster, beyond the key's cone, has a picture light of its own and gets at least a quarter of the left
    // one's.
    const auto lightAt = [&scene](const glm::vec3& point, const glm::vec3& normal) {
        glm::vec3 color(0.0f);
        float total = 0.0f;
        for (const rv::scene::Light& light : scene.lights) {
            const float received = irradiance(light, point, normal);
            color += light.color * received;
            total += received;
        }
        return std::pair(color, total);
    };
    const auto tungstenDominant = [](const glm::vec3& color) { return color.r > color.g && color.g > color.b; };
    const SceneObject* posterLeft = scene.findObject("poster_left");
    const SceneObject* posterRight = scene.findObject("poster_right");
    REQUIRE(posterLeft != nullptr);
    REQUIRE(posterRight != nullptr);
    const glm::vec3 wallFront(0.0f, 0.0f, 0.02f); // the front of the frames, 2 cm off the wall
    const auto [leftColor, leftLight] = lightAt(translationOf(posterLeft->transform) + wallFront, south);
    const auto [rightColor, rightLight] = lightAt(translationOf(posterRight->transform) + wallFront, south);
    const glm::vec3 between(0.5f * (screen.x + translationOf(posterRight->transform).x), 1.85f, -4.999f);
    CHECK(tungstenDominant(leftColor));
    CHECK(tungstenDominant(rightColor));
    CHECK(tungstenDominant(lightAt(between, south).first));
    CHECK(rightLight > 0.25f * leftLight);
    // Each shelf of the collection has a warm-white ceiling spot of its own aimed at it (museum display lighting, style
    // 4.1 accent lights), which keeps its middle in tungsten-dominant light and at least a fifth of the main shelf's
    // (which the key lights too).
    const auto [mainColor, mainLight] = lightAt(translationOf(shelf->transform) + glm::vec3(0.0f, 1.0f, 0.2f), south);
    for (const SceneObject* collection : shelves) {
        INFO(collection->id);
        const glm::vec3 middle = translationOf(collection->transform) + glm::vec3(0.0f, 1.0f, 0.2f);
        const bool spotted = std::any_of(scene.lights.begin(), scene.lights.end(), [&](const rv::scene::Light& light) {
            return &light != &key && light.type == LightType::Spot && coneFactor(light, middle) > 0.9f &&
                   std::abs(light.position.x - middle.x) < 0.3f && light.position.y > wallHeight - 0.1f;
        });
        CHECK(spotted);
        const auto [color, received] = lightAt(middle, south);
        CHECK(received > 0.2f * mainLight);
        CHECK(tungstenDominant(color));
    }

    // Ambient occlusion with the defaults of the plan: radius 0.4 m, intensity 1.
    CHECK(scene.environment.ssao.enabled);
    CHECK(scene.environment.ssao.radius == 0.4f);
    CHECK(scene.environment.ssao.intensity == 1.0f);
}

TEST_CASE("scene/reference_scenes: m0_testroom cartridges sit at the contract slot positions of its shelves",
          "[scene][reference_scenes][m0_testroom]") {
    const SceneDescription scene = loadReference("m0_testroom");
    const auto shelves = objectsOfKind(scene, "shelf");
    REQUIRE(shelves.size() == 2);
    for (const SceneObject* shelf : shelves) {
        INFO(shelf->id);
        CHECK(ShelfLayout::fromParams(shelf->params).slotsPerLevel() == 8);
    }

    const auto cartridges = objectsOfKind(scene, "cartridge");
    REQUIRE(cartridges.size() == 96);
    std::set<std::tuple<std::string, int, int>> usedSlots;
    std::set<std::string> titles;
    std::set<std::uint32_t> seeds;
    for (const SceneObject* cartridge : cartridges) {
        INFO("cartridge " << cartridge->id);
        const glm::vec3 size = cartridge->params.contains("size")
                                   ? rv::core::jsonToVec3(cartridge->params.at("size"), cartridge->id).value()
                                   : kDefaultCartridgeSize;

        // The cartridge pivot is its center: half its height above the slot's bottom-center point of one shelf, upright
        // with the label facing out of that shelf (+Z), like the shelf itself.
        const glm::vec3 center = translationOf(cartridge->transform);
        int matches = 0;
        for (const SceneObject* shelf : shelves) {
            const ShelfLayout layout = ShelfLayout::fromParams(shelf->params);
            for (int level = 0; level < layout.levels; ++level) {
                for (int index = 0; index < layout.slotsPerLevel(); ++index) {
                    const glm::vec3 local =
                        layout.slotBottomCenter(level, index) + glm::vec3(0.0f, 0.5f * size.y, 0.0f);
                    const glm::vec3 expected(shelf->transform * glm::vec4(local, 1.0f));
                    if (!rv::core::nearlyEqual(center, expected, 1e-4f)) {
                        continue;
                    }
                    ++matches;
                    CHECK(rv::core::nearlyEqual(size, layout.slotSize));
                    for (int column = 0; column < 3; ++column) {
                        CHECK(rv::core::nearlyEqual(glm::vec3(cartridge->transform[column]),
                                                    glm::vec3(shelf->transform[column])));
                    }
                    CHECK(usedSlots.insert({shelf->id, level, index}).second); // one cartridge per slot
                    CHECK(level > 0); // the bottom level stays free for the collection to grow (style 5.3)
                }
            }
        }
        CHECK(matches == 1);

        CHECK(cartridge->materialNameForRole("plastic_shell") == "MAT_Plastic_Gray");
        CHECK(cartridge->materialNameForRole("label") == "MAT_Label_Print");
        const TextureSource& label = cartridge->texturesByRole.at("label");
        CHECK(label.kind == TextureSource::Kind::Pattern);
        CHECK(label.pattern == "cover");
        CHECK_FALSE(label.title.empty());
        titles.insert(label.title);
        seeds.insert(label.seed);
    }
    CHECK(usedSlots.size() == 96);
    CHECK(titles.size() == 96); // every cover is distinct
    CHECK(seeds.size() == 96);
    CHECK(titles.contains("SUPER PLACEHOLDER WORLD"));
}

TEST_CASE("scene/reference_scenes: m0_testroom keeps 1.2 m free passages", "[scene][reference_scenes][m0_testroom]") {
    const SceneDescription scene = loadReference("m0_testroom");
    constexpr float kPassage = 1.2f;
    constexpr float kAgainstWall = 0.5f; // closer than this to a wall = placed against it, not a passage
    constexpr float kHalfWidth = 4.5f;
    constexpr float kHalfDepth = 5.0f;

    // Floor-standing colliders as their world boxes (the box around a turned piece's footprint: conservative).
    struct Piece {
        std::string id;
        rv::core::Aabb box;
        bool againstWall = false;
    };
    std::vector<Piece> furniture;
    rv::scene::CollisionWorld world;
    for (const SceneObject& object : scene.objects) {
        if (!object.collides || object.kind == "room_shell" || translationOf(object.transform).y != 0.0f) {
            continue;
        }
        INFO("object " << object.id);
        const auto prefab = rv::geometry::buildPrefab(object.kind, object.params);
        REQUIRE(prefab.has_value());
        Piece piece{object.id, prefab->bounds.transformed(object.transform)};
        for (const float wallGap : {piece.box.min.x + kHalfWidth, kHalfWidth - piece.box.max.x,
                                    piece.box.min.z + kHalfDepth, kHalfDepth - piece.box.max.z}) {
            CHECK((wallGap < kAgainstWall || wallGap >= kPassage));
            CHECK(wallGap >= 0.0f); // inside the room
            piece.againstWall = piece.againstWall || wallGap < kAgainstWall;
        }
        world.addBox(piece.box, object.id);
        furniture.push_back(std::move(piece));
    }
    // Two shelves, the TV cabinet, the sideboard, the floor lamp and the armchair.
    REQUIRE(furniture.size() == 6);
    // Between two pieces a walker passes 1.2 m wide, unless both stand against walls (side by side along a wall they
    // leave a niche, not a way: the two shelves form one unit, the lamp stands in the corner beside them).
    for (std::size_t i = 0; i < furniture.size(); ++i) {
        for (std::size_t j = i + 1; j < furniture.size(); ++j) {
            const Piece& a = furniture[i];
            const Piece& b = furniture[j];
            INFO(a.id << " <-> " << b.id);
            const float dx = std::max({0.0f, b.box.min.x - a.box.max.x, a.box.min.x - b.box.max.x});
            const float dz = std::max({0.0f, b.box.min.z - a.box.max.z, a.box.min.z - b.box.max.z});
            CHECK((std::sqrt(dx * dx + dz * dz) >= kPassage || (a.againstWall && b.againstWall)));
        }
    }

    // The walker spawns free, at the scene camera and at the start of the walk scripts (--spawn 0,0,3.2,0,-6), and can
    // walk from the doorway to the front of the hero wall.
    const rv::scene::Capsule capsule;
    CHECK_FALSE(world.overlaps(glm::vec3(scene.camera.position.x, 0.0f, scene.camera.position.z), capsule));
    const glm::vec3 scripted(0.0f, 0.0f, 3.2f);
    CHECK_FALSE(world.overlaps(scripted, capsule));
    const glm::vec3 heroWall = world.moveCapsule(scripted, glm::vec3(0.0f, 0.0f, -8.0f), capsule);
    CHECK(heroWall.z < -4.0f);
}
