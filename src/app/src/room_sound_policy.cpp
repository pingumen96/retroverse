#include <rv/app/room_sound_policy.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>

#include <glm/geometric.hpp>

#include <rv/app/audio_policy.hpp>
#include <rv/audio/spatial.hpp>

namespace rv::app {
namespace {

/// The first area whose boxes hold `point`.
std::optional<std::size_t> areaHolding(const scene::SceneDescription& description, const glm::vec3& point) {
    for (std::size_t index = 0; index < description.areas.size(); ++index) {
        if (description.areas[index].contains(point)) {
            return index;
        }
    }
    return std::nullopt;
}

glm::vec3 positionOf(const scene::SceneObject& object) {
    return glm::vec3(object.transform[3]);
}

void addEmitter(std::vector<RoomToneEmitter>& emitters, audio::SoundId sound, const glm::vec3& position, double level,
                float packGain, bool whileShowing, std::optional<std::size_t> area, std::string at) {
    if (!(packGain > 0.0f)) {
        return;
    }
    emitters.push_back(
        RoomToneEmitter{sound, position, level * static_cast<double>(packGain), whileShowing, area, std::move(at)});
}

} // namespace

std::optional<scene::SoundSetup> roomSoundOf(const scene::SceneDescription& description,
                                             std::optional<std::size_t> area) {
    if (area && *area < description.areas.size() && description.areas[*area].environment.sound) {
        return description.areas[*area].environment.sound;
    }
    return description.environment.sound;
}

std::vector<RoomToneEmitter> roomToneEmitters(const scene::SceneDescription& description) {
    std::vector<RoomToneEmitter> emitters;
    // Without areas the scene's tone, its centre that of its objects.
    core::Aabb extent;
    for (const scene::SceneObject& object : description.objects) {
        const glm::vec3 position = positionOf(object);
        extent.expand(core::Aabb::fromMinMax(position, position));
        const std::string_view id = object.id;
        const bool crt = id.ends_with("fx:hero.crt");
        const bool sign = id.ends_with("fx:hero.sign");
        if (!crt && !sign) {
            continue;
        }
        const std::optional<std::size_t> area =
            description.areas.empty() ? std::nullopt : areaHolding(description, position);
        if (!description.areas.empty() && !area) {
            continue;
        }
        const std::optional<scene::SoundSetup> sound = roomSoundOf(description, area);
        if (!sound) {
            continue;
        }
        if (crt) {
            const glm::vec3 at = position + glm::vec3(0.0f, 0.25f, 0.0f);
            addEmitter(emitters, audio::SoundId::CrtHum, at, kHumLevel, sound->hum, false, area, object.id);
            addEmitter(emitters, audio::SoundId::PhosphorHiss, at, kHissLevel, sound->hiss, true, area, object.id);
        } else {
            addEmitter(emitters, audio::SoundId::NeonBuzz, position, kBuzzLevel, sound->buzz, false, area, object.id);
        }
    }
    const auto addCentre = [&](std::optional<std::size_t> area, const glm::vec3& centre, const std::string& name) {
        const std::optional<scene::SoundSetup> sound = roomSoundOf(description, area);
        if (!sound) {
            return;
        }
        const glm::vec3 at(centre.x, 1.6f, centre.z);
        addEmitter(emitters, audio::SoundId::RoomMurmur, at, kMurmurLevel, sound->murmur, false, area, name);
        addEmitter(emitters, audio::SoundId::ClockTick, at, kClockLevel, sound->clock, false, area, name);
    };
    if (description.areas.empty()) {
        if (!extent.isEmpty()) {
            addCentre(std::nullopt, extent.center(), "centre");
        }
    } else {
        for (std::size_t index = 0; index < description.areas.size(); ++index) {
            const scene::SceneArea& area = description.areas[index];
            if (!area.boxes.empty()) {
                addCentre(index, area.boxes.front().center(), area.id + "/centre");
            }
        }
    }
    return emitters;
}

double roomToneDistanceGain(double distanceMeters) noexcept {
    if (!(distanceMeters > kRoomToneFullMeters)) {
        return 1.0;
    }
    return std::max(kRoomToneFloor, kRoomToneFullMeters / distanceMeters);
}

RoomToneMix roomToneMix(const RoomToneEmitter& emitter, const glm::vec3& eye, const glm::vec3& forward,
                        std::optional<std::size_t> eyeArea, bool heroShowing, bool clipHeard) noexcept {
    if ((emitter.area && emitter.area != eyeArea) || (emitter.whileShowing && !heroShowing)) {
        return {};
    }
    const glm::vec3 offset = emitter.position - eye;
    const double distance = static_cast<double>(glm::length(offset));
    double gain = emitter.gain * roomToneDistanceGain(distance);
    if (clipHeard) {
        gain *= audio::gainFromDecibels(kRoomToneUnderClipDecibels);
    }
    const double azimuth = azimuthOf(eye.x, eye.z, forward.x, forward.z, emitter.position.x, emitter.position.z);
    return RoomToneMix{gain, audio::panForAzimuth(azimuth)};
}

std::vector<FloorPatch> floorPatches(const scene::SceneDescription& description,
                                     std::span<const core::Aabb> localBounds) {
    std::vector<FloorPatch> patches;
    for (std::size_t index = 0; index < description.objects.size() && index < localBounds.size(); ++index) {
        const scene::SceneObject& object = description.objects[index];
        if (object.source != scene::SceneObject::Source::Prefab || object.kind != "rug" ||
            localBounds[index].isEmpty()) {
            continue;
        }
        const core::Aabb world = localBounds[index].transformed(object.transform);
        const bool runner = object.id.find("dressing:runner") != std::string::npos;
        patches.push_back(FloorPatch{glm::vec2(world.min.x, world.min.z), glm::vec2(world.max.x, world.max.z),
                                     runner ? FloorSurface::Runner : FloorSurface::Rug});
    }
    return patches;
}

FloorSurface surfaceUnder(const glm::vec2& feet, std::span<const FloorPatch> patches) noexcept {
    FloorSurface surface = FloorSurface::Parquet;
    for (const FloorPatch& patch : patches) {
        if (feet.x >= patch.min.x && feet.x <= patch.max.x && feet.y >= patch.min.y && feet.y <= patch.max.y) {
            surface = patch.surface;
        }
    }
    return surface;
}

audio::SoundId footstepSound(FloorSurface surface) noexcept {
    switch (surface) {
    case FloorSurface::Rug:
        return audio::SoundId::FootstepRug;
    case FloorSurface::Runner:
        return audio::SoundId::FootstepRunner;
    case FloorSurface::Parquet:
        break;
    }
    return audio::SoundId::FootstepParquet;
}

std::optional<FootstepCadence::Step> FootstepCadence::advance(const glm::vec2& feet, double seconds,
                                                              bool walking) noexcept {
    if (!walking) {
        m_last.reset();
        return std::nullopt;
    }
    if (!m_last) {
        // Setting off: the first step half a stride on.
        m_last = feet;
        m_travelled = 0.5 * kWalkStrideMeters;
        return std::nullopt;
    }
    const double moved = static_cast<double>(glm::length(feet - *m_last));
    m_last = feet;
    if (moved > kTeleportMeters) {
        m_travelled = 0.5 * kWalkStrideMeters;
        return std::nullopt;
    }
    if (moved <= 0.0) {
        return std::nullopt;
    }
    const double speed = seconds > 0.0 ? moved / seconds : 0.0;
    const double stride = speed > kSprintMetersPerSecond ? kSprintStrideMeters : kWalkStrideMeters;
    m_travelled += moved;
    if (m_travelled < stride) {
        return std::nullopt;
    }
    m_travelled = std::fmod(m_travelled - stride, stride);
    m_left = !m_left;
    ++m_steps;
    return Step{m_left};
}

std::optional<audio::SoundId> interactionSound(WorldState before, WorldState after) noexcept {
    if (before == after) {
        return std::nullopt;
    }
    if ((before == WorldState::FreeRoam && after == WorldState::Focus) ||
        (before == WorldState::Focus && after == WorldState::FreeRoam)) {
        return audio::SoundId::ItemClack;
    }
    if ((before == WorldState::Focus && after == WorldState::Inspect) ||
        (before == WorldState::Inspect && after == WorldState::Focus)) {
        return audio::SoundId::CaseOpen;
    }
    if (after == WorldState::Launching) {
        return audio::SoundId::InsertionClick;
    }
    return std::nullopt;
}

} // namespace rv::app
