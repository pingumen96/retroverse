#include <rv/geometry/socket.hpp>

#include <cmath>

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

} // namespace rv::geometry
