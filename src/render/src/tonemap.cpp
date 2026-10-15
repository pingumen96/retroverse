#include <rv/render/tonemap.hpp>

#include <algorithm>

#include <glm/mat3x3.hpp>

namespace rv::gl {
namespace {

glm::mat3 fromColumns(const std::array<float, 9>& c) noexcept {
    return glm::mat3(c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8]);
}

float curveFit(float v) noexcept {
    const AcesCurveFit& k = kAcesCurveFit;
    return (v * (v + k.a) - k.b) / (v * (k.c * v + k.d) + k.e);
}

} // namespace

glm::vec3 tonemapAcesFitted(const glm::vec3& color) noexcept {
    glm::vec3 v = fromColumns(kAcesInputColumns) * color;
    v = glm::vec3(curveFit(v.x), curveFit(v.y), curveFit(v.z));
    v = fromColumns(kAcesOutputColumns) * v;
    return glm::clamp(v, glm::vec3(0.0f), glm::vec3(1.0f));
}

glm::vec3 tonemap(const glm::vec3& radiance, float exposure) noexcept {
    glm::vec3 exposed = radiance * exposure;
    for (int i = 0; i < 3; ++i) {
        // !(x > 0) is true for NaN as well.
        exposed[i] = !(exposed[i] > 0.0f) ? 0.0f : std::min(exposed[i], kTonemapInputLimit);
    }
    return tonemapAcesFitted(exposed);
}

} // namespace rv::gl
