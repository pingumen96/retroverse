#include <rv/preview/preview_scene.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <fmt/format.h>

#include <rv/app/model_viewer.hpp>
#include <rv/app/scene_content_plan.hpp>
#include <rv/core/text.hpp>

namespace rv::preview {
namespace {

using RoleMaterials = std::map<std::string, std::string>;

/// A cover picture: the pattern of the test room's labels and posters.
scene::TextureSource cover(std::string title, std::uint32_t seed) {
    scene::TextureSource source;
    source.kind = scene::TextureSource::Kind::Pattern;
    source.pattern = "cover";
    source.title = std::move(title);
    source.seed = seed;
    return source;
}

} // namespace

std::map<std::string, std::string> prefabRoleMaterials(std::string_view kind) {
    if (kind == "room_shell") {
        return RoleMaterials{{"floor", "MAT_Floor_OakPlanks"},
                             {"wall", "MAT_Wall_Painted"},
                             {"ceiling", "MAT_Wall_Painted"},
                             {"trim", "MAT_Wood_Walnut"},
                             {"wainscot", "MAT_Wood_Varnished"}};
    }
    if (kind == "shelf") {
        return RoleMaterials{{"wood_primary", "MAT_Wood_Walnut"}, {"wood_secondary", "MAT_Wood_Varnished"}};
    }
    if (kind == "cartridge") {
        return RoleMaterials{{"plastic_shell", "MAT_Plastic_Gray"}, {"label", "MAT_Label_Print"}};
    }
    if (kind == "crt") {
        return RoleMaterials{{"plastic_body", "MAT_Plastic_AgedIvory"},
                             {"plastic_bezel", "MAT_Plastic_Black"},
                             {"screen", std::string(kScreenMaterialName)},
                             {"plastic_dark", "MAT_Plastic_Black"}};
    }
    if (kind == "tv_cabinet") {
        return RoleMaterials{{"wood_primary", "MAT_Wood_Walnut"},
                             {"wood_secondary", "MAT_Wood_Varnished"},
                             {"metal_trim", "MAT_Metal_Brushed"}};
    }
    if (kind == "console_cartridge") {
        return RoleMaterials{{"plastic_body", "MAT_Plastic_Gray"},
                             {"plastic_dark", "MAT_Plastic_Black"},
                             {"plastic_accent", "MAT_Plastic_Accent"}};
    }
    if (kind == "poster_frame") {
        return RoleMaterials{{"frame", "MAT_Wood_Oak"}, {"picture", "MAT_Paper_Print"}};
    }
    if (kind == "rug") {
        return RoleMaterials{{"fabric", "MAT_Carpet_Ornate"}};
    }
    if (kind == "floor_lamp") {
        return RoleMaterials{{"metal", "MAT_Metal_Brushed"}, {"lamp_shade", "MAT_Lamp_Shade"}};
    }
    if (kind == "armchair") {
        return RoleMaterials{{"fabric", "MAT_Fabric_Sofa"}, {"wood", "MAT_Wood_Walnut"}};
    }
    return {};
}

std::map<std::string, scene::TextureSource> prefabRoleTextures(std::string_view kind) {
    std::map<std::string, scene::TextureSource> textures;
    if (kind == "crt") {
        scene::TextureSource bars;
        bars.kind = scene::TextureSource::Kind::Pattern;
        bars.pattern = std::string(kScreenPattern);
        textures.emplace("screen", std::move(bars));
    } else if (kind == "cartridge") {
        textures.emplace("label", cover("PREVIEW CARTRIDGE", 7));
    } else if (kind == "poster_frame") {
        textures.emplace("picture", cover("PREVIEW POSTER", 11));
    }
    return textures;
}

scene::SceneDescription makePreviewScene(const PreviewSubject& subject) {
    scene::SceneObject object;
    if (subject.model || !subject.modelFile.empty()) {
        object.source = scene::SceneObject::Source::Model;
        object.modelPath = core::pathToUtf8(subject.modelFile);
    } else if (!subject.prefabKind.empty()) {
        object.source = scene::SceneObject::Source::Prefab;
        object.kind = subject.prefabKind;
        object.params = subject.params;
        object.materialsByRole = prefabRoleMaterials(subject.prefabKind);
        object.texturesByRole = prefabRoleTextures(subject.prefabKind);
    } else {
        // Nothing that can be built (a manifest that names neither a model nor a generator): a model object without a
        // model, which buildSceneGeometry replaces with the placeholder.
        object.source = scene::SceneObject::Source::Model;
        object.modelPath = subject.title;
    }
    scene::SceneDescription scene = app::makeInspectionScene(std::move(object), subject.bounds);
    scene.name = "asset_preview";
    if (!subject.target) {
        scene.objects.clear(); // the empty studio, waiting for a file
    }
    return scene;
}

scene::ModelLoader previewModelLoader(const PreviewSubject& subject) {
    return [model = subject.model](std::string_view path) -> Expected<std::shared_ptr<const assets::ModelData>> {
        if (model) {
            return model;
        }
        return makeError(ErrorCode::NotFound, "the model could not be loaded (see the warnings)", std::string(path));
    };
}

std::vector<ScreenSurface> screenSurfaces(const PreviewSubject& subject) {
    std::vector<ScreenSurface> screens;
    for (const geometry::Socket& socket : subject.sockets) {
        if (socket.kind != geometry::SocketKind::ScreenSurface || !socket.meshIndex) {
            continue;
        }
        ScreenSurface screen;
        screen.socketId = socket.id;
        screen.nodeName = socket.nodeName;
        screen.bounds = socket.bounds;
        screen.transform = socket.transform;
        const std::uint32_t first = *socket.meshIndex;
        if (subject.model) {
            const assets::ModelData& model = *subject.model;
            // The node that instances the screen's mesh: its primitives and its placement.
            const auto node = std::find_if(model.nodes.begin(), model.nodes.end(), [&](const assets::ModelNode& n) {
                return n.name == socket.nodeName && n.mesh == first;
            });
            if (node == model.nodes.end() || first >= model.meshes.size()) {
                continue;
            }
            for (std::uint32_t mesh = first; mesh < first + node->meshCount && mesh < model.meshes.size(); ++mesh) {
                screen.modelMeshes.push_back(mesh);
            }
            screen.patternSize = app::patternSizeFor(model.meshes[first], node->world);
        } else if (subject.prefab && first < subject.prefab->parts.size()) {
            screen.patternSize = app::patternSizeFor(subject.prefab->parts[first].mesh, glm::mat4(1.0f));
        } else {
            continue;
        }
        screens.push_back(std::move(screen));
    }
    return screens;
}

std::string aspectRatioName(float aspect) {
    if (!std::isfinite(aspect) || aspect <= 0.0f) {
        return "-";
    }
    constexpr std::array<std::pair<std::string_view, float>, 7> kRatios{{{"1:1", 1.0f},
                                                                         {"5:4", 5.0f / 4.0f},
                                                                         {"4:3", 4.0f / 3.0f},
                                                                         {"3:2", 3.0f / 2.0f},
                                                                         {"16:10", 16.0f / 10.0f},
                                                                         {"16:9", 16.0f / 9.0f},
                                                                         {"21:9", 21.0f / 9.0f}}};
    for (const auto& [name, ratio] : kRatios) {
        if (std::abs(aspect - ratio) <= 0.015f * ratio) {
            return std::string(name);
        }
    }
    return fmt::format("{:.2f}:1", aspect);
}

std::optional<float> parseAspectRatio(std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    float width = 0.0f;
    float height = 0.0f;
    const std::string_view widthText = text.substr(0, colon);
    const std::string_view heightText = text.substr(colon + 1);
    const auto parsedWidth = std::from_chars(widthText.data(), widthText.data() + widthText.size(), width);
    const auto parsedHeight = std::from_chars(heightText.data(), heightText.data() + heightText.size(), height);
    if (parsedWidth.ec != std::errc{} || parsedWidth.ptr != widthText.data() + widthText.size() ||
        parsedHeight.ec != std::errc{} || parsedHeight.ptr != heightText.data() + heightText.size() ||
        !std::isfinite(width) || !std::isfinite(height) || width <= 0.0f || height <= 0.0f) {
        return std::nullopt;
    }
    return width / height;
}

} // namespace rv::preview
