#include <rv/app/content_consoles.hpp>

#include <algorithm>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

namespace rv::app {

std::vector<ConsoleSite> consoleSites(const scene::SceneDescription& description) {
    std::vector<ConsoleSite> sites;
    for (const scene::SceneObject& object : description.objects) {
        if (object.source != scene::SceneObject::Source::Prefab) {
            continue;
        }
        const std::size_t slash = object.id.find('/');
        if (slash == std::string::npos || std::string_view(object.id).substr(slash + 1) != kHeroConsoleId) {
            continue;
        }
        auto room = world::RoomId::parse(std::string_view(object.id).substr(0, slash));
        if (!room || room->kind() != world::RoomIdKind::System) {
            continue;
        }
        sites.push_back(ConsoleSite{*room, object.id, object.transform});
    }
    return sites;
}

std::string contentModelPath(std::string_view system, std::string_view modelFile) {
    return std::string(kContentModelPrefix) + std::string(assets::kConsolesFolder) + "/" + std::string(system) + "/" +
           std::string(modelFile);
}

std::optional<std::string> contentModelSystem(std::string_view modelPath) {
    const std::string prefix = std::string(kContentModelPrefix) + std::string(assets::kConsolesFolder) + "/";
    if (!modelPath.starts_with(prefix)) {
        return std::nullopt;
    }
    const std::string_view rest = modelPath.substr(prefix.size());
    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0) {
        return std::nullopt;
    }
    return std::string(rest.substr(0, slash));
}

glm::mat4 consoleModelLocal(const assets::ModelData& model, const assets::ConsoleManifest& manifest,
                            float width) noexcept {
    // Left by what reaches past the cabinet's right edge.
    const float shift = std::min(0.0F, kConsoleRightReach - 0.5F * width);
    return glm::translate(glm::mat4(1.0F), glm::vec3(shift, 0.0F, 0.0F)) *
           assets::consolePlacement(model, manifest.front);
}

scene::SceneObject consoleModelObject(const ConsoleSite& site, std::string modelPath, const glm::mat4& local) {
    scene::SceneObject object;
    object.id = site.objectId;
    object.source = scene::SceneObject::Source::Model;
    object.modelPath = std::move(modelPath);
    object.transform = site.transform * local;
    object.castsShadows = true;
    object.collides = true;
    return object;
}

ConsoleSocket consoleModelSocket(const assets::ModelData& model, const assets::ConsoleManifest& manifest,
                                 const ConsoleSite& site, float width, std::optional<glm::mat4> heroScreen) {
    const assets::ConsoleMediaSocket media = assets::consoleMediaSocket(model, manifest);
    // consoleMediaSocket() is in the console's frame (the placement applied); the shift alone remains.
    const float shift = std::min(0.0F, kConsoleRightReach - 0.5F * width);
    const glm::mat4 toSite = glm::translate(glm::mat4(1.0F), glm::vec3(shift, 0.0F, 0.0F));
    return ConsoleSocket{site.transform * toSite * media.transform, media.opening, heroScreen};
}

} // namespace rv::app
