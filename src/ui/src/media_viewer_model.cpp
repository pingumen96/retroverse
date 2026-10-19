#include <rv/ui/media_viewer_model.hpp>

#include <algorithm>
#include <utility>

namespace rv::ui {

std::string_view toString(PictureState state) noexcept {
    switch (state) {
    case PictureState::Loading:
        return "loading";
    case PictureState::Shown:
        return "shown";
    case PictureState::Failed:
        return "failed";
    }
    return "unknown";
}

void MediaViewerModel::open(std::string gameId, std::string title, std::string platform,
                            std::span<const std::string> kinds) {
    m_open = true;
    m_gameId = std::move(gameId);
    m_title = std::move(title);
    m_platform = std::move(platform);
    m_pages.clear();
    m_index = 0;
    for (const std::string_view kind : media_viewer::kKinds) {
        if (std::find(kinds.begin(), kinds.end(), kind) != kinds.end()) {
            m_pages.push_back(MediaPage{std::string(kind)});
        }
    }
}

void MediaViewerModel::close() noexcept {
    m_open = false;
}

ViewerStep MediaViewerModel::apply(UiCommand command) noexcept {
    if (!m_open) {
        return ViewerStep::None;
    }
    switch (command) {
    case UiCommand::Back:
        return ViewerStep::Close;
    case UiCommand::Left:
    case UiCommand::PageUp:
    case UiCommand::FilterPrevious:
        return m_index > 0 ? select(m_index - 1) : ViewerStep::None;
    case UiCommand::Right:
    case UiCommand::PageDown:
    case UiCommand::FilterNext:
    case UiCommand::Activate:
        return select(m_index + 1);
    case UiCommand::Home:
        return select(0);
    case UiCommand::End:
        return m_pages.empty() ? ViewerStep::None : select(m_pages.size() - 1);
    default:
        return ViewerStep::None;
    }
}

ViewerStep MediaViewerModel::select(std::size_t index) noexcept {
    if (!m_open || index >= m_pages.size() || index == m_index) {
        return ViewerStep::None;
    }
    m_index = index;
    return ViewerStep::Moved;
}

void MediaViewerModel::setPicture(std::size_t index, PictureState state, int width, int height) noexcept {
    if (index >= m_pages.size()) {
        return;
    }
    MediaPage& page = m_pages[index];
    page.state = state;
    page.width = state == PictureState::Shown ? width : 0;
    page.height = state == PictureState::Shown ? height : 0;
}

const MediaPage* MediaViewerModel::current() const noexcept {
    return m_index < m_pages.size() ? &m_pages[m_index] : nullptr;
}

std::optional<std::size_t> MediaViewerModel::indexOf(std::string_view kind) const noexcept {
    for (std::size_t index = 0; index < m_pages.size(); ++index) {
        if (m_pages[index].kind == kind) {
            return index;
        }
    }
    return std::nullopt;
}

PictureFit fitPicture(float stageWidth, float stageHeight, int pictureWidth, int pictureHeight) noexcept {
    if (!(stageWidth > 0.0f) || !(stageHeight > 0.0f) || pictureWidth <= 0 || pictureHeight <= 0) {
        return {};
    }
    const float width = static_cast<float>(pictureWidth);
    const float height = static_cast<float>(pictureHeight);
    const float scale = std::min({stageWidth / width, stageHeight / height, media_viewer::kMaxEnlargement});
    return PictureFit{width * scale, height * scale};
}

} // namespace rv::ui
