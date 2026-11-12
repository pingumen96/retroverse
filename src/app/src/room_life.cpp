#include "room_life.hpp"

#include <array>
#include <string_view>

#include <spdlog/spdlog.h>

#include <rv/app/room_life.hpp>
#include <rv/core/seed.hpp>

namespace rv::app {
namespace {

/// The materials that live (I12 step 8): the media racks' activity LEDs blink (eight slots a second, an off slot at
/// 15 % of the light), the lava lamps' wax breathes (a breath every 12.5 s down to 55 %).
struct LivingMaterial {
    std::string_view name;
    gl::FlickerMode mode;
    float rate;
    float depth;
};
constexpr std::array<LivingMaterial, 3> kLivingMaterials{{
    {"MAT_Led_Activity_Red", gl::FlickerMode::Blink, 1.0f, 0.85f},
    {"MAT_Led_Activity_Green", gl::FlickerMode::Blink, 0.8f, 0.85f},
    {"MAT_Lava_Wax", gl::FlickerMode::Pulse, 0.8f, 0.45f},
}};

} // namespace

void RoomLife::rescan(const Frame& frame) {
    m_dirty = false;
    m_reduceMotion = frame.reduceMotion;
    for (const scene::MaterialHandle handle : m_flickering) {
        if (handle.value < frame.materials->size()) {
            (void)frame.materials->setFlicker(handle, std::nullopt);
        }
    }
    m_flickering.clear();
    m_blinking = 0;
    m_breathing = 0;
    for (const LivingMaterial& living : kLivingMaterials) {
        const std::optional<scene::MaterialHandle> handle = frame.materials->find(living.name);
        if (!handle) {
            continue; // no piece of the scene uses it
        }
        gl::EmissiveFlicker flicker;
        flicker.seed = static_cast<std::uint32_t>(core::deriveSeed("life", living.name) & 0xFFFFFFu);
        flicker.rate = frame.reduceMotion ? 0.0f : living.rate;
        flicker.depth = living.depth;
        flicker.mode = living.mode;
        if (auto set = frame.materials->setFlicker(*handle, flicker); !set) {
            spdlog::warn("World: {} cannot {}: {}", living.name,
                         living.mode == gl::FlickerMode::Blink ? "blink" : "breathe", set.error().toString());
            continue;
        }
        m_flickering.push_back(*handle);
        (living.mode == gl::FlickerMode::Blink ? m_blinking : m_breathing) += 1;
    }
    m_spinning = 0;
    m_clockHands = 0;
    for (const SceneInstance::MovingPart& part : frame.instance->movingParts()) {
        (part.motion.kind == scene::ObjectMotion::Kind::Spin ? m_spinning : m_clockHands) += 1;
    }
}

void RoomLife::update(const Frame& frame) {
    if (frame.instance == nullptr || frame.materials == nullptr) {
        return;
    }
    if (m_dirty || m_reduceMotion != frame.reduceMotion) {
        rescan(frame);
    }
    const std::size_t moved = frame.instance->moveParts([&](const SceneInstance::MovingPart& part) {
        return lifeAngle(part.motion, frame.seconds, frame.clockSeconds, frame.reduceMotion);
    });
    m_movedFrames += moved > 0 ? 1 : 0;
}

} // namespace rv::app
