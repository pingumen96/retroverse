#include <rv/world/world_plan.hpp>

#include <algorithm>
#include <functional>
#include <tuple>

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

namespace {

/// std::sort of a list, unless it is in strictly ascending order already (m3/preview-time: a plan read back is sorted,
/// and a regeneration sorts its whole world): such a list has one sorted order, the one it is in, so the result is
/// the sort's whatever the list holds; a list with two elements in one place is sorted as before.
template <class Range, class Less = std::less<>>
void sortList(Range& range, Less less = {}) {
    const auto unordered =
        std::adjacent_find(range.begin(), range.end(), [&](const auto& a, const auto& b) { return !less(a, b); });
    if (unordered != range.end()) {
        std::sort(range.begin(), range.end(), less);
    }
}

} // namespace

void sortPlan(WorldPlan& plan) {
    sortList(plan.floors,
             [](const Floor& a, const Floor& b) { return std::tie(a.order, a.id) < std::tie(b.order, b.id); });
    sortList(plan.rooms, [](const Room& a, const Room& b) { return a.id < b.id; });
    for (Fixture& fixture : plan.fixtures) {
        sortList(fixture.levels);
    }
    sortList(plan.fixtures, [](const Fixture& a, const Fixture& b) { return a.id < b.id; });
    sortList(plan.placements);
    sortList(plan.dressing);
    sortList(plan.lights, [](const PlanLight& a, const PlanLight& b) { return a.id < b.id; });
    sortList(plan.unplaced);
}

bool isSha256Hex(std::string_view text) noexcept {
    return text.size() == 64 && std::all_of(text.begin(), text.end(),
                                            [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

} // namespace rv::world
