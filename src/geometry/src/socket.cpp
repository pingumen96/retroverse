#include <rv/geometry/socket.hpp>

#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <system_error>

namespace rv::geometry {

std::string_view toString(SocketKind kind) noexcept {
    switch (kind) {
    case SocketKind::Anchor:
        return "anchor";
    case SocketKind::ScreenSurface:
        return "screen-surface";
    case SocketKind::ColliderProxy:
        return "collider-proxy";
    case SocketKind::ShelfSlot:
        return "shelf-slot";
    case SocketKind::Other:
        return "other";
    }
    return "other";
}

const Socket* findSocket(std::span<const Socket> sockets, std::string_view id) noexcept {
    for (const Socket& socket : sockets) {
        if (socket.id == id) {
            return &socket;
        }
    }
    return nullptr;
}

glm::vec3 socketSize(const Socket& socket) noexcept {
    if (socket.bounds.isEmpty()) {
        return glm::vec3(0.0f);
    }
    const glm::vec3 axes(glm::length(glm::vec3(socket.transform[0])), glm::length(glm::vec3(socket.transform[1])),
                         glm::length(glm::vec3(socket.transform[2])));
    const glm::vec3 size = socket.bounds.size() * axes;
    const bool finite = std::isfinite(size.x) && std::isfinite(size.y) && std::isfinite(size.z);
    return finite ? size : glm::vec3(0.0f);
}

namespace {

/// A decimal number in int range without sign or leading zeros (but "0" itself), the whole of `text`.
std::optional<int> parseIndex(std::string_view text) noexcept {
    if (text.empty() || (text.size() > 1 && text.front() == '0') || text.front() < '0' || text.front() > '9') {
        return std::nullopt;
    }
    int value = 0;
    const char* end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::optional<SlotAddress> parseSlotId(std::string_view id) noexcept {
    constexpr std::string_view kPrefix = "Slot_";
    if (!id.starts_with(kPrefix)) {
        return std::nullopt;
    }
    id.remove_prefix(kPrefix.size());
    const std::size_t separator = id.find('_');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    const std::optional<int> level = parseIndex(id.substr(0, separator));
    const std::optional<int> index = parseIndex(id.substr(separator + 1));
    if (!level || !index) {
        return std::nullopt;
    }
    return SlotAddress{*level, *index};
}

std::optional<int> parseLevelId(std::string_view id) noexcept {
    constexpr std::string_view kPrefix = "Level_";
    if (!id.starts_with(kPrefix)) {
        return std::nullopt;
    }
    return parseIndex(id.substr(kPrefix.size()));
}

std::vector<SlotLevel> slotLevels(std::span<const Socket> sockets) {
    struct Found {
        int count = 0;
        int firstIndex = std::numeric_limits<int>::max();
        const Socket* first = nullptr;
    };
    std::map<int, Found> levels;
    for (const Socket& socket : sockets) {
        if (socket.kind != SocketKind::ShelfSlot) {
            continue;
        }
        const std::optional<SlotAddress> address = parseSlotId(socket.id);
        if (!address) {
            continue;
        }
        Found& found = levels[address->level];
        ++found.count;
        if (address->index < found.firstIndex) {
            found.firstIndex = address->index;
            found.first = &socket;
        }
    }
    std::vector<SlotLevel> result;
    result.reserve(levels.size());
    for (const auto& [level, found] : levels) {
        SlotLevel entry;
        entry.level = level;
        entry.count = found.count;
        entry.slotSize = socketSize(*found.first);
        entry.height = found.first->transform[3].y;
        result.push_back(entry);
    }
    return result;
}

} // namespace rv::geometry
