#include <rv/core/change_watcher.hpp>

namespace rv::core {

std::string_view toString(WatchMode mode) noexcept {
    switch (mode) {
    case WatchMode::Arming:
        return "arming";
    case WatchMode::Notify:
        return "notify";
    case WatchMode::Poll:
        return "poll";
    case WatchMode::Lost:
        return "lost";
    }
    return "lost";
}

std::string_view toString(ChangeKind kind) noexcept {
    switch (kind) {
    case ChangeKind::Added:
        return "added";
    case ChangeKind::Removed:
        return "removed";
    case ChangeKind::Modified:
        return "modified";
    case ChangeKind::RenamedFrom:
        return "renamed-from";
    case ChangeKind::RenamedTo:
        return "renamed-to";
    case ChangeKind::Polled:
        return "polled";
    case ChangeKind::Overflow:
        return "overflow";
    case ChangeKind::RootLost:
        return "root-lost";
    case ChangeKind::RootBack:
        return "root-back";
    }
    return "modified";
}

} // namespace rv::core
