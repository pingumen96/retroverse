#include <rv/ui/focus_regions.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace rv::ui {
namespace {

[[nodiscard]] std::size_t indexOf(NavDirection direction) noexcept {
    return static_cast<std::size_t>(direction);
}

} // namespace

FocusRegions::Region& FocusRegions::regionNamed(std::string_view name) {
    const auto found =
        std::find_if(m_regions.begin(), m_regions.end(), [name](const Region& region) { return region.name == name; });
    if (found != m_regions.end()) {
        return *found;
    }
    Region& created = m_regions.emplace_back();
    created.name = std::string(name);
    return created;
}

const FocusRegions::Region* FocusRegions::find(std::string_view name) const noexcept {
    const auto found =
        std::find_if(m_regions.begin(), m_regions.end(), [name](const Region& region) { return region.name == name; });
    return found != m_regions.end() ? &*found : nullptr;
}

void FocusRegions::link(std::string_view region, NavDirection direction, std::string_view to) {
    static_cast<void>(regionNamed(to)); // the target region exists from now on too
    regionNamed(region).links[indexOf(direction)] = std::string(to);
}

void FocusRegions::setEntry(std::string_view region, std::string_view element) {
    Region& named = regionNamed(region);
    if (named.entry != element) {
        named.entry = std::string(element);
    }
}

void FocusRegions::onFocused(const FocusTarget& target) {
    Region& named = regionNamed(target.region);
    if (named.remembered != target.element) {
        named.remembered = target.element;
    }
}

void FocusRegions::forget(std::string_view region) {
    for (Region& named : m_regions) {
        if (named.name == region) {
            named.remembered.clear();
        }
    }
}

std::optional<std::string> FocusRegions::linked(std::string_view region, NavDirection direction) const {
    const Region* found = find(region);
    if (found == nullptr || found->links[indexOf(direction)].empty()) {
        return std::nullopt;
    }
    return found->links[indexOf(direction)];
}

std::optional<std::string> FocusRegions::landing(std::string_view region) const {
    if (std::optional<std::string> element = remembered(region)) {
        return element;
    }
    return entry(region);
}

std::optional<std::string> FocusRegions::remembered(std::string_view region) const {
    const Region* found = find(region);
    if (found == nullptr || found->remembered.empty()) {
        return std::nullopt;
    }
    return found->remembered;
}

std::optional<std::string> FocusRegions::entry(std::string_view region) const {
    const Region* found = find(region);
    if (found == nullptr || found->entry.empty()) {
        return std::nullopt;
    }
    return found->entry;
}

std::optional<FocusTarget> FocusRegions::across(std::string_view region, NavDirection direction) const {
    std::optional<std::string> to = linked(region, direction);
    if (!to) {
        return std::nullopt;
    }
    std::optional<std::string> element = landing(*to);
    if (!element) {
        return std::nullopt;
    }
    return FocusTarget{std::move(*to), std::move(*element)};
}

} // namespace rv::ui
