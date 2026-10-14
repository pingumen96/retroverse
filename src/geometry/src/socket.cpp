#include <rv/geometry/socket.hpp>

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

} // namespace rv::geometry
