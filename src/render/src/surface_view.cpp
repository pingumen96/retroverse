#include <rv/render/surface_view.hpp>

namespace rv::gl {

std::string_view toString(SurfaceView view) noexcept {
    switch (view) {
    case SurfaceView::Lit:
        return "Lit";
    case SurfaceView::BaseColor:
        return "Base color";
    case SurfaceView::Normals:
        return "Normals";
    case SurfaceView::UvChecker:
        return "UV checker";
    }
    return "Lit";
}

std::string_view inspectionDefine(SurfaceView view) noexcept {
    switch (view) {
    case SurfaceView::Lit:
        return {};
    case SurfaceView::BaseColor:
        return "RV_INSPECT_BASE_COLOR";
    case SurfaceView::Normals:
        return "RV_INSPECT_NORMALS";
    case SurfaceView::UvChecker:
        return "RV_INSPECT_UV";
    }
    return {};
}

std::string_view inspectionProgramName(SurfaceView view) noexcept {
    switch (view) {
    case SurfaceView::Lit:
        return {};
    case SurfaceView::BaseColor:
        return "inspect_base_color";
    case SurfaceView::Normals:
        return "inspect_normals";
    case SurfaceView::UvChecker:
        return "inspect_uv_checker";
    }
    return {};
}

} // namespace rv::gl
