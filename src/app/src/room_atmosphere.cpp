#include "room_atmosphere.hpp"

#include <spdlog/spdlog.h>

#include <rv/app/identity_signs.hpp>
#include <rv/core/seed.hpp>
#include <rv/scene/atmosphere.hpp>

#include "screen_service.hpp"
#include "world_signs.hpp"

namespace rv::app {
namespace {

constexpr std::string_view kCrtGlowLight = "accent:crt_glow";
constexpr std::string_view kHeroCrt = "fx:hero.crt";
/// A dip takes this share of the sign's light.
constexpr float kFlickerDepth = 0.6f;

std::optional<scene::AtmosphereSetup> atmosphereOfRoom(const scene::SceneDescription& description,
                                                       std::string_view area) {
    const std::optional<std::size_t> index = area.empty() ? std::nullopt : description.findArea(area);
    return scene::atmosphereOf(description, index);
}

} // namespace

void RoomAtmosphere::rescan(const Frame& frame) {
    m_dirty = false;
    m_reduceMotion = frame.reduceMotion;
    // The signs: those flickering before steady again (a handle may be another material's now: steady is harmless),
    // then the hero signs of the rooms whose air says so.
    for (const scene::MaterialHandle handle : m_flickered) {
        if (handle.value < frame.materials->size()) {
            (void)frame.materials->setFlicker(handle, std::nullopt);
        }
    }
    m_flickered.clear();
    for (const IdentitySignTarget& target : identitySignTargets(*frame.description)) {
        if (target.place != IdentitySignPlace::Hero) {
            continue;
        }
        const std::string room = target.room.toString();
        const std::optional<scene::AtmosphereSetup> air = atmosphereOfRoom(*frame.description, room);
        if (!air || !air->neonFlicker) {
            continue;
        }
        const std::optional<scene::MaterialHandle> handle =
            objectPictureMaterial(target.objectId, *frame.instance, *frame.materials);
        if (!handle) {
            continue;
        }
        gl::EmissiveFlicker flicker;
        flicker.seed = static_cast<std::uint32_t>(core::deriveSeed("flicker", room) & 0xFFFFFFu);
        flicker.rate = frame.reduceMotion ? 0.0f : 1.0f;
        flicker.depth = kFlickerDepth;
        if (auto set = frame.materials->setFlicker(*handle, flicker); !set) {
            spdlog::warn("World: the sign {} cannot flicker: {}", target.objectId, set.error().toString());
            continue;
        }
        m_flickered.push_back(*handle);
    }
    // The glows: each room's crt glow whose air breathes, from its light as it stands now.
    std::vector<Glow> glows;
    const std::span<const scene::Light> lights = frame.instance->sceneLights();
    for (std::size_t index = 0; index < lights.size(); ++index) {
        const scene::Light& light = lights[index];
        if (!light.name.ends_with(kCrtGlowLight)) {
            continue;
        }
        const std::optional<scene::AtmosphereSetup> air = atmosphereOfRoom(*frame.description, light.area);
        if (!air || !air->crtBreathing) {
            continue;
        }
        Glow glow;
        glow.prefix = light.name.substr(0, light.name.size() - kCrtGlowLight.size());
        glow.light = index;
        glow.name = light.name;
        // A glow found again keeps its base (its light may hold the values written).
        const auto before = std::find_if(m_glows.begin(), m_glows.end(),
                                         [&](const Glow& candidate) { return candidate.name == light.name; });
        glow.base = before != m_glows.end() ? before->base : CrtGlow{light.color, light.intensity};
        glow.current = CrtGlow{light.color, light.intensity};
        glow.written = glow.current;
        glows.push_back(std::move(glow));
    }
    m_glows = std::move(glows);
}

bool RoomAtmosphere::write(SceneInstance& instance, Glow& glow) {
    const std::span<scene::Light> lights = instance.sceneLights();
    if (glow.light >= lights.size() || lights[glow.light].name != glow.name || glow.written == glow.current) {
        return false;
    }
    lights[glow.light].color = glow.current.color;
    lights[glow.light].intensity = glow.current.intensity;
    glow.written = glow.current;
    return true;
}

void RoomAtmosphere::update(const Frame& frame) {
    if (frame.description == nullptr || frame.instance == nullptr || frame.materials == nullptr) {
        return;
    }
    if (m_dirty || m_reduceMotion != frame.reduceMotion) {
        rescan(frame);
    }
    if (m_glows.empty()) {
        return;
    }
    // The room whose CRT the hero screen is.
    std::string heroPrefix;
    std::optional<std::array<float, 3>> picture;
    if (frame.screens != nullptr) {
        const std::string hero = frame.screens->screenId();
        if (hero.ends_with(kHeroCrt)) {
            heroPrefix = hero.substr(0, hero.size() - kHeroCrt.size());
            picture = frame.screens->heroPictureColour();
        }
    }
    const bool firstFrame = picture.has_value() && !m_hadPicture;
    m_hadPicture = picture.has_value();
    m_sinceWrite += std::max(frame.seconds, 0.0f);
    const bool due = m_sinceWrite >= kCrtGlowUpdateSeconds || firstFrame || frame.reduceMotion || frame.settle;
    bool changed = false;
    for (Glow& glow : m_glows) {
        // ADR-0021 §4: under reduceMotion the glow keeps the light's own colour.
        const bool hero = glow.prefix == heroPrefix && picture.has_value() && !frame.reduceMotion;
        const CrtGlow target = hero ? crtGlowFor(glow.base, *picture) : glow.base;
        // At once on a clip's first frame (and back at once when it stops), under reduceMotion, and for a room that
        // is not the hero's.
        const bool instant = frame.reduceMotion || frame.settle || (hero && firstFrame) || !hero;
        glow.current = easedCrtGlow(glow.current, target, frame.seconds, instant);
        if ((due || !hero) && write(*frame.instance, glow)) {
            changed = true;
        }
    }
    if (due) {
        m_sinceWrite = 0.0f;
    }
    if (changed) {
        frame.instance->repackLights();
        ++m_glowWrites;
    }
}

} // namespace rv::app
