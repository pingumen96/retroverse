#include <rv/app/crt_looks.hpp>

#include <cstddef>
#include <utility>

#include <rv/app/cabinet_screens.hpp>
#include <rv/world/room_look.hpp>

namespace rv::app {
namespace {

/// The preset `name` names, if any.
std::optional<assets::CrtPreset> presetNamed(std::string_view name) noexcept {
    return name.empty() ? std::nullopt : assets::crtPresetFromName(name);
}

/// The object id of a screen's material instance key ("<object id>/screen", then "@<w>x<h>" for a pattern), if it is
/// one.
std::optional<std::string_view> screenObjectOf(std::string_view key) noexcept {
    constexpr std::string_view kRole = "/screen";
    const std::size_t at = key.rfind(kRole);
    if (at == std::string_view::npos || at == 0) {
        return std::nullopt;
    }
    const std::string_view rest = key.substr(at + kRole.size());
    if (!rest.empty() && rest.front() != '@') {
        return std::nullopt; // another role ("/screen_back")
    }
    return key.substr(0, at);
}

} // namespace

std::string_view toString(CrtRule rule) noexcept {
    switch (rule) {
    case CrtRule::Kind:
        return "kind";
    case CrtRule::Pack:
        return "pack";
    case CrtRule::VideoSettings:
        return "video.json";
    }
    return "kind";
}

assets::CrtPreset defaultCrtPreset(ScreenKind kind) noexcept {
    return kind == ScreenKind::Cabinet ? assets::CrtPreset::Arcade : assets::CrtPreset::Consumer;
}

assets::CrtPreset strongerCrtPreset(assets::CrtPreset preset) noexcept {
    switch (preset) {
    case assets::CrtPreset::Monitor:
        return assets::CrtPreset::Consumer;
    case assets::CrtPreset::Consumer:
        return assets::CrtPreset::Arcade;
    case assets::CrtPreset::Arcade:
    case assets::CrtPreset::Neutral:
        break;
    }
    return preset;
}

CrtChoice chooseCrtPreset(CrtEffect effect, std::string_view videoPreset, std::string_view packPreset,
                          ScreenKind kind) noexcept {
    CrtChoice choice;
    if (const auto video = presetNamed(videoPreset)) {
        choice.chosen = *video;
        choice.rule = CrtRule::VideoSettings;
    } else if (const auto pack = presetNamed(packPreset)) {
        choice.chosen = *pack;
        choice.rule = CrtRule::Pack;
    } else {
        choice.chosen = defaultCrtPreset(kind);
        choice.rule = CrtRule::Kind;
    }
    switch (effect) {
    case CrtEffect::Off:
        choice.preset = assets::CrtPreset::Neutral;
        break;
    case CrtEffect::Vintage:
        choice.preset = strongerCrtPreset(choice.chosen);
        break;
    case CrtEffect::Light:
        choice.preset = choice.chosen;
        break;
    }
    return choice;
}

std::string screenRoomOf(std::string_view objectId, std::string_view sceneName) {
    const std::size_t slash = objectId.find('/');
    return std::string(slash == std::string_view::npos || slash == 0 ? sceneName : objectId.substr(0, slash));
}

std::optional<ScreenKind> screenKindOf(const scene::SceneObject& object, bool firstCrt) {
    const bool heroRole = (object.kind == "crt" || object.kind == kCabinetPrefabKind) &&
                          object.id.ends_with(kHeroCrtIdSuffix);
    if (heroRole || (object.kind == "crt" && firstCrt)) {
        return ScreenKind::Hero;
    }
    if (object.kind == "crt" || object.kind == world::kSecondaryTvKind) {
        return ScreenKind::Secondary;
    }
    return std::nullopt;
}

void ScreenLooks::setScene(std::map<std::string, std::string, std::less<>> packPresets) {
    m_packPresets = std::move(packPresets);
    m_applied.clear();
}

CrtChoice ScreenLooks::choose(ScreenKind kind, std::string_view roomId, std::string_view screenId,
                              std::optional<std::string_view> packPreset) const {
    std::string_view pack;
    if (packPreset) {
        pack = *packPreset;
    } else if (const auto found = m_packPresets.find(roomId); found != m_packPresets.end()) {
        pack = found->second;
    }
    const ScreenSettings settings = m_video.screen(kind, roomId, screenId);
    return chooseCrtPreset(m_video.crtEffect, settings.crtPreset, pack, kind);
}

void ScreenLooks::record(ScreenLookApplied applied) {
    std::string key = applied.screen;
    m_applied.insert_or_assign(std::move(key), std::move(applied));
}

std::vector<ScreenLookApplied> ScreenLooks::applied() const {
    std::vector<ScreenLookApplied> looks;
    looks.reserve(m_applied.size());
    for (const auto& [screen, look] : m_applied) {
        looks.push_back(look);
    }
    return looks;
}

std::size_t applyScreenLooks(SceneContentPlan& plan, std::span<const scene::SceneObject> objects,
                             std::string_view sceneName, ScreenLooks& looks) {
    // A hand-made scene's hero is its first crt, unless an object holds the hero's role (findScreens()).
    bool heroByRole = false;
    for (const scene::SceneObject& object : objects) {
        if ((object.kind == "crt" || object.kind == kCabinetPrefabKind) && object.id.ends_with(kHeroCrtIdSuffix)) {
            heroByRole = true;
            break;
        }
    }
    std::optional<std::size_t> firstCrt;
    if (!heroByRole) {
        for (std::size_t index = 0; index < objects.size(); ++index) {
            if (objects[index].kind == "crt") {
                firstCrt = index;
                break;
            }
        }
    }
    // Each material instance's object: the first item drawn with it (an instance is one object's part).
    std::vector<std::optional<std::size_t>> objectOf(plan.materials.size());
    for (const PlannedItem& item : plan.items) {
        if (item.material < objectOf.size() && !objectOf[item.material]) {
            objectOf[item.material] = item.object;
        }
    }
    std::size_t changed = 0;
    for (std::size_t index = 0; index < plan.materials.size(); ++index) {
        PlannedMaterial& planned = plan.materials[index];
        if (!planned.material.crt || !planned.picture || !objectOf[index] || *objectOf[index] >= objects.size()) {
            continue;
        }
        const scene::SceneObject& object = objects[*objectOf[index]];
        const std::optional<std::string_view> id = screenObjectOf(planned.key);
        if (!id || *id != object.id) {
            continue;
        }
        const std::optional<ScreenKind> kind = screenKindOf(object, firstCrt == *objectOf[index]);
        if (!kind) {
            continue;
        }
        const std::string room = screenRoomOf(object.id, sceneName);
        const CrtChoice choice = looks.choose(*kind, room, object.id);
        planned.material.crt = looks.effect(choice);
        looks.record(ScreenLookApplied{object.id, room, *kind, choice});
        ++changed;
    }
    return changed;
}

} // namespace rv::app
