#include <rv/world/world_plan.hpp>

#include <algorithm>

namespace rv::world {

std::string GeneratorVersion::toString() const {
    return std::to_string(major) + "." + std::to_string(minor);
}

std::optional<GeneratorVersion> GeneratorVersion::parse(std::string_view text) noexcept {
    const std::size_t dot = text.find('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    const auto major = detail::parseDecimal(text.substr(0, dot), 999'999);
    const auto minor = detail::parseDecimal(text.substr(dot + 1), 999'999);
    if (!major || !minor) {
        return std::nullopt;
    }
    return GeneratorVersion{*major, *minor};
}

void sortPlan(WorldPlan& plan) {
    std::sort(plan.rooms.begin(), plan.rooms.end(), [](const Room& a, const Room& b) { return a.id < b.id; });
    for (Fixture& fixture : plan.fixtures) {
        std::sort(fixture.levels.begin(), fixture.levels.end());
    }
    std::sort(plan.fixtures.begin(), plan.fixtures.end(),
              [](const Fixture& a, const Fixture& b) { return a.id < b.id; });
    std::sort(plan.placements.begin(), plan.placements.end());
    std::sort(plan.dressing.begin(), plan.dressing.end());
    std::sort(plan.unplaced.begin(), plan.unplaced.end());
}

bool isSha256Hex(std::string_view text) noexcept {
    return text.size() == 64 && std::all_of(text.begin(), text.end(),
                                            [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

} // namespace rv::world
