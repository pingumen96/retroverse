#include <rv/app/picture_bounds.hpp>

#include <algorithm>
#include <cmath>

namespace rv::app {

namespace {

bool lit(std::span<const std::uint8_t> rgba, std::size_t pixel) noexcept {
    return rgba[pixel * 4] > kBorderBlackLevel || rgba[pixel * 4 + 1] > kBorderBlackLevel ||
           rgba[pixel * 4 + 2] > kBorderBlackLevel;
}

/// The lines of a bar of `count` lines out of `total`, less the content's edge, none when thinner than kMinBarShare.
int bar(int count, int total) noexcept {
    const int kept = std::max(count - 1, 0);
    return static_cast<float>(kept) < kMinBarShare * static_cast<float>(total) ? 0 : kept;
}

bool sameCrop(const glm::vec4& a, const glm::vec4& b) noexcept {
    constexpr float same = PictureBounds::kSameCrop;
    return std::abs(a.x - b.x) <= same && std::abs(a.y - b.y) <= same && std::abs(a.x + a.z - b.x - b.z) <= same &&
           std::abs(a.y + a.w - b.y - b.w) <= same;
}

/// `outer` holds `inner` (within kSameCrop).
bool holds(const glm::vec4& outer, const glm::vec4& inner) noexcept {
    constexpr float same = PictureBounds::kSameCrop;
    return inner.x >= outer.x - same && inner.y >= outer.y - same && inner.x + inner.z <= outer.x + outer.z + same &&
           inner.y + inner.w <= outer.y + outer.w + same;
}

} // namespace

std::optional<glm::vec4> findPictureBounds(std::span<const std::uint8_t> rgba, glm::ivec2 size) noexcept {
    if (size.x <= 0 || size.y <= 0 ||
        rgba.size() < static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) * 4) {
        return std::nullopt;
    }
    const auto at = [size](int x, int y) {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(size.x) + static_cast<std::size_t>(x);
    };
    const auto rowBar = [&](int y) {
        int count = 0;
        for (int x = 0; x < size.x; ++x) {
            count += lit(rgba, at(x, y)) ? 1 : 0;
        }
        return static_cast<float>(count) <= kBarLitShare * static_cast<float>(size.x);
    };
    const auto columnBar = [&](int x) {
        int count = 0;
        for (int y = 0; y < size.y; ++y) {
            count += lit(rgba, at(x, y)) ? 1 : 0;
        }
        return static_cast<float>(count) <= kBarLitShare * static_cast<float>(size.y);
    };
    int top = 0;
    while (top < size.y && rowBar(top)) {
        ++top;
    }
    if (top == size.y) {
        return std::nullopt; // all dark
    }
    int bottom = 0;
    while (rowBar(size.y - 1 - bottom)) {
        ++bottom;
    }
    int left = 0;
    while (left < size.x && columnBar(left)) {
        ++left;
    }
    if (left == size.x) {
        return std::nullopt;
    }
    int right = 0;
    while (columnBar(size.x - 1 - right)) {
        ++right;
    }
    // Centred: the thinner bar of each pair on both sides.
    const int vertical = bar(std::min(top, bottom), size.y);
    const int horizontal = bar(std::min(left, right), size.x);
    const glm::vec2 total(size);
    const glm::vec4 crop(static_cast<float>(horizontal) / total.x, static_cast<float>(vertical) / total.y,
                         static_cast<float>(size.x - 2 * horizontal) / total.x,
                         static_cast<float>(size.y - 2 * vertical) / total.y);
    if (crop.z < kMinCropShare || crop.w < kMinCropShare) {
        return std::nullopt;
    }
    return crop;
}

bool PictureBounds::update(const std::optional<glm::vec4>& found) noexcept {
    if (!found || sameCrop(*found, m_crop)) {
        m_candidate.reset();
        m_candidateSamples = 0;
        return false;
    }
    // Content in a bar the crop leaves out: widened at once (the game's picture is never cut for long).
    if (!holds(m_crop, *found)) {
        m_crop = *found;
        m_candidate.reset();
        m_candidateSamples = 0;
        return true;
    }
    if (m_candidate && sameCrop(*m_candidate, *found)) {
        ++m_candidateSamples;
    } else {
        m_candidate = *found;
        m_candidateSamples = 1;
    }
    if (m_candidateSamples < kNarrowSamples) {
        return false;
    }
    m_crop = *m_candidate;
    m_candidate.reset();
    m_candidateSamples = 0;
    return true;
}

void PictureBounds::reset() noexcept {
    *this = PictureBounds{};
}

} // namespace rv::app
