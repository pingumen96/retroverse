#include <rv/scene/orbit_controller.hpp>

#include <algorithm>
#include <cmath>

#include <rv/core/math.hpp>

namespace rv::scene {
namespace {

/// Smallest radius framed, so that a flat or point-like model still gets a sensible camera.
constexpr float kMinRadius = 0.0005f;
/// Closest the eye comes to the target, as a share of the framed radius, and in meters.
constexpr float kMinDistanceRadii = 0.05f;
constexpr float kMinDistanceMeters = 0.001f;
/// Farthest the eye goes, in framed radii and at least in meters.
constexpr float kMaxDistanceRadii = 100.0f;
constexpr float kMaxDistanceMeters = 10.0f;
/// Clip planes: the near plane a quarter of the way to the front of the framed sphere, within these bounds; the far
/// plane far behind it.
constexpr float kMinNearPlane = 0.0005f;
constexpr float kMaxNearPlane = 0.05f;
constexpr float kMinFarPlane = 20.0f;
constexpr float kFarPlaneFactor = 50.0f;

/// Unit vector the eye looks along at `yaw` and `pitch` (Camera conventions).
glm::vec3 forwardOf(float yaw, float pitch) noexcept {
    return glm::vec3(std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch));
}

bool finite(const glm::vec2& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

} // namespace

void OrbitController::frame(const core::Aabb& bounds, float yawDeg, float pitchDeg, const CameraLens& lens,
                            float aspect, const ViewArea& area) noexcept {
    const bool usable = !bounds.isEmpty() && core::isFinite(bounds.min) && core::isFinite(bounds.max);
    const core::Aabb framed =
        usable ? bounds : core::Aabb::fromMinMax(glm::vec3(-0.5f, 0.0f, -0.5f), glm::vec3(0.5f, 1.0f, 0.5f));
    m_target = framed.center();
    m_radius = std::max(0.5f * glm::length(framed.size()), kMinRadius);
    setAngles(core::degreesToRadians(yawDeg), core::degreesToRadians(pitchDeg));

    // The sphere fits the narrower side of the area.
    const ViewArea frameArea = usableViewArea(area);
    const glm::vec2 half = 0.5f * (frameArea.max - frameArea.min);
    const float horizontalFov = core::degreesToRadians(lens.horizontalFovDeg);
    const float tanHalfHorizontal = std::tan(0.5f * horizontalFov);
    const float tanHalfVertical = std::tan(0.5f * Camera::verticalFovRadians(horizontalFov, aspect));
    const float halfAngle = std::atan(std::min(half.x * tanHalfHorizontal, half.y * tanHalfVertical));
    const float fit = halfAngle > 0.0f ? m_radius / std::sin(halfAngle) : m_radius;
    m_distance = std::clamp(m_settings.frameMargin * fit, minDistance(), maxDistance());
}

void OrbitController::update(const OrbitInput& input, float deltaSeconds, const Camera& view,
                             float viewportHeight) noexcept {
    const float dt = std::isfinite(deltaSeconds) ? std::max(deltaSeconds, 0.0f) : 0.0f;
    float yaw = m_yaw;
    float pitch = m_pitch;
    if (finite(input.orbitPixels)) {
        const float perPixel = core::degreesToRadians(m_settings.degreesPerPixel);
        yaw += input.orbitPixels.x * perPixel;
        pitch -= input.orbitPixels.y * perPixel; // dragging down raises the eye
    }
    if (finite(input.orbitRate)) {
        const float step = core::degreesToRadians(m_settings.stickDegreesPerSecond) * dt;
        yaw += input.orbitRate.x * step;
        pitch -= input.orbitRate.y * step;
    }
    if (m_autoRotate) {
        yaw += core::degreesToRadians(m_settings.autoRotateDegreesPerSecond) * dt;
    }
    setAngles(yaw, pitch);

    // Zoom factors multiply: their logarithms add, and an extreme sum saturates at the distance limits.
    float logZoom = 0.0f;
    if (std::isfinite(input.zoomNotches)) {
        logZoom -= input.zoomNotches * std::log(m_settings.zoomPerNotch);
    }
    if (std::isfinite(input.zoomRate)) {
        logZoom -= input.zoomRate * dt * std::log(m_settings.zoomPerSecond);
    }
    if (std::isfinite(logZoom)) {
        m_distance = std::clamp(m_distance * std::exp(logZoom), minDistance(), maxDistance());
    }

    // Panning moves the target across the view, in the eye's right and up directions, so that a drag keeps the target
    // under the pointer at its own depth.
    const glm::vec3 forward = forwardOf(m_yaw, m_pitch);
    const glm::vec3 right(std::cos(m_yaw), 0.0f, std::sin(m_yaw));
    const glm::vec3 up = glm::cross(right, forward);
    const float viewHeight = 2.0f * m_distance * std::tan(0.5f * view.verticalFovRadians());
    glm::vec3 pan(0.0f);
    if (finite(input.panPixels) && std::isfinite(viewportHeight) && viewportHeight > 0.0f) {
        const float metersPerPixel = viewHeight / viewportHeight;
        pan += (-right * input.panPixels.x + up * input.panPixels.y) * metersPerPixel;
    }
    if (finite(input.panRate)) {
        pan += (right * input.panRate.x + up * input.panRate.y) * (m_settings.panViewsPerSecond * viewHeight * dt);
    }
    if (core::isFinite(pan)) {
        m_target += pan;
    }
}

void OrbitController::apply(Camera& camera, const ViewArea& area) const noexcept {
    camera.setLens(lens(camera.lens().horizontalFovDeg));
    const glm::vec3 position = eye();
    const ViewAngles angles = aimAt(position, m_target, camera.lens(), camera.aspect(), area, m_yaw);
    camera.setPose(position, angles.yaw, angles.pitch);
}

CameraLens OrbitController::lens(float horizontalFovDeg) const noexcept {
    CameraLens lens;
    lens.horizontalFovDeg = horizontalFovDeg;
    lens.nearPlane = std::clamp(0.25f * (m_distance - m_radius), kMinNearPlane, kMaxNearPlane);
    lens.farPlane = std::max(kMinFarPlane, kFarPlaneFactor * (m_distance + m_radius));
    return lens;
}

glm::vec3 OrbitController::eye() const noexcept {
    return m_target - forwardOf(m_yaw, m_pitch) * m_distance;
}

float OrbitController::minDistance() const noexcept {
    return std::max(kMinDistanceRadii * m_radius, kMinDistanceMeters);
}

float OrbitController::maxDistance() const noexcept {
    return std::max(kMaxDistanceRadii * m_radius, kMaxDistanceMeters);
}

void OrbitController::setAngles(float yaw, float pitch) noexcept {
    if (std::isfinite(yaw)) {
        m_yaw = std::remainder(yaw, core::kTwoPi);
    }
    if (std::isfinite(pitch)) {
        const float limit = core::degreesToRadians(m_settings.pitchLimitDeg);
        m_pitch = std::clamp(pitch, -limit, limit);
    }
}

} // namespace rv::scene
