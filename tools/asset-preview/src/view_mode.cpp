#include <rv/preview/view_mode.hpp>

namespace rv::preview {

std::string_view toString(ViewMode mode) noexcept {
    switch (mode) {
    case ViewMode::Lit:
        return "Lit";
    case ViewMode::BaseColor:
        return "Base color";
    case ViewMode::Normals:
        return "Normals";
    case ViewMode::UvChecker:
        return "UV checker";
    case ViewMode::Wireframe:
        return "Wireframe";
    }
    return "Lit";
}

std::string_view viewModeName(ViewMode mode) noexcept {
    switch (mode) {
    case ViewMode::Lit:
        return "lit";
    case ViewMode::BaseColor:
        return "base-color";
    case ViewMode::Normals:
        return "normals";
    case ViewMode::UvChecker:
        return "uv-checker";
    case ViewMode::Wireframe:
        return "wireframe";
    }
    return "lit";
}

std::optional<ViewMode> viewModeFromName(std::string_view name) noexcept {
    for (const ViewMode mode : kViewModes) {
        if (viewModeName(mode) == name) {
            return mode;
        }
    }
    return std::nullopt;
}

gl::SurfaceView surfaceViewOf(ViewMode mode) noexcept {
    switch (mode) {
    case ViewMode::BaseColor:
        return gl::SurfaceView::BaseColor;
    case ViewMode::Normals:
    case ViewMode::Wireframe: // bright edges whatever the material, colored by the way they face
        return gl::SurfaceView::Normals;
    case ViewMode::UvChecker:
        return gl::SurfaceView::UvChecker;
    case ViewMode::Lit:
        break;
    }
    return gl::SurfaceView::Lit;
}

bool wireframeOf(ViewMode mode) noexcept {
    return mode == ViewMode::Wireframe;
}

} // namespace rv::preview
