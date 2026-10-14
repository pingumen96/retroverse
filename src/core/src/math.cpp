#include <rv/core/math.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace rv::core {

bool nearlyEqual(float a, float b, float epsilon) noexcept {
    const float diff = std::fabs(a - b);
    if (diff <= epsilon) {
        return true;
    }
    return diff <= epsilon * std::max(std::fabs(a), std::fabs(b));
}

bool nearlyEqual(const glm::vec3& a, const glm::vec3& b, float epsilon) noexcept {
    return nearlyEqual(a.x, b.x, epsilon) && nearlyEqual(a.y, b.y, epsilon) && nearlyEqual(a.z, b.z, epsilon);
}

bool nearlyEqual(const glm::vec4& a, const glm::vec4& b, float epsilon) noexcept {
    return nearlyEqual(a.x, b.x, epsilon) && nearlyEqual(a.y, b.y, epsilon) && nearlyEqual(a.z, b.z, epsilon) &&
           nearlyEqual(a.w, b.w, epsilon);
}

bool nearlyEqual(const glm::mat4& a, const glm::mat4& b, float epsilon) noexcept {
    for (int column = 0; column < 4; ++column) {
        if (!nearlyEqual(a[column], b[column], epsilon)) {
            return false;
        }
    }
    return true;
}

bool isFinite(const glm::vec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool isFinite(const glm::vec4& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
}

bool isFinite(const glm::mat4& m) noexcept {
    for (int column = 0; column < 4; ++column) {
        if (!isFinite(m[column])) {
            return false;
        }
    }
    return true;
}

float wrapAngleRadians(float radians) noexcept {
    if (!std::isfinite(radians)) {
        return 0.0f;
    }
    float wrapped = std::fmod(radians + kPi, kTwoPi);
    if (wrapped <= 0.0f) {
        wrapped += kTwoPi;
    }
    return wrapped - kPi;
}

float srgbToLinear(float encoded) noexcept {
    const float c = std::clamp(encoded, 0.0f, 1.0f);
    if (c <= 0.04045f) {
        return c / 12.92f;
    }
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float linear) noexcept {
    const float c = std::clamp(linear, 0.0f, 1.0f);
    if (c <= 0.0031308f) {
        return c * 12.92f;
    }
    return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

glm::vec3 srgbToLinear(const glm::vec3& encoded) noexcept {
    return {srgbToLinear(encoded.x), srgbToLinear(encoded.y), srgbToLinear(encoded.z)};
}

glm::vec3 linearToSrgb(const glm::vec3& linear) noexcept {
    return {linearToSrgb(linear.x), linearToSrgb(linear.y), linearToSrgb(linear.z)};
}

namespace {

int hexDigit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

} // namespace

Expected<glm::vec4> parseHexColor(std::string_view text) {
    std::string_view digits = text;
    if (!digits.empty() && digits.front() == '#') {
        digits.remove_prefix(1);
    }
    if (digits.size() != 6 && digits.size() != 8) {
        return makeError(ErrorCode::ParseError, "expected #RRGGBB or #RRGGBBAA", std::string(text));
    }
    float components[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const std::size_t count = digits.size() / 2;
    for (std::size_t i = 0; i < count; ++i) {
        const int high = hexDigit(digits[2 * i]);
        const int low = hexDigit(digits[2 * i + 1]);
        if (high < 0 || low < 0) {
            return makeError(ErrorCode::ParseError, "invalid hexadecimal digit", std::string(text));
        }
        components[i] = static_cast<float>(high * 16 + low) / 255.0f;
    }
    return glm::vec4(components[0], components[1], components[2], components[3]);
}

Expected<glm::vec3> hexToLinearRgb(std::string_view text) {
    auto color = parseHexColor(text);
    if (!color) {
        return propagate(color.error());
    }
    return srgbToLinear(glm::vec3(*color));
}

} // namespace rv::core
