#include <rv/ui/coverage_model.hpp>

#include <algorithm>
#include <utility>

#include <fmt/format.h>

namespace rv::ui {

std::string_view toString(CoverageState state) noexcept {
    switch (state) {
    case CoverageState::Loading:
        return "loading";
    case CoverageState::Listed:
        return "listed";
    case CoverageState::Empty:
        return "empty";
    case CoverageState::Unavailable:
        return "unavailable";
    }
    return "unknown";
}

std::string_view toString(CoverageView view) noexcept {
    return view == CoverageView::System ? "system" : "systems";
}

std::string_view toString(CoverageTab tab) noexcept {
    return tab == CoverageTab::Scraped ? "scraped" : "games";
}

std::optional<CoverageTab> coverageTabFromName(std::string_view name) noexcept {
    for (const CoverageTab tab : kCoverageTabs) {
        if (toString(tab) == name) {
            return tab;
        }
    }
    return std::nullopt;
}

std::string shareText(std::optional<double> share) {
    if (!share) {
        return "-";
    }
    std::string text = fmt::format("{:.1f}", std::clamp(*share, 0.0, 1.0) * 100.0);
    std::replace(text.begin(), text.end(), '.', ',');
    return text;
}

void CoverageModel::open() {
    m_open = true;
    m_state = CoverageState::Loading;
    m_view = CoverageView::Systems;
    m_tab = CoverageTab::Games;
    m_focused = 0;
    m_focusedName.clear();
    m_content = {};
    m_request = CoverageCommand::Load;
    ++m_revision;
}

void CoverageModel::close() noexcept {
    if (m_open) {
        m_open = false;
        ++m_revision;
    }
}

std::optional<CoverageCommand> CoverageModel::takeRequest() noexcept {
    return std::exchange(m_request, std::nullopt);
}

const CoverageRow* CoverageModel::system() const noexcept {
    const std::vector<CoverageRow>& shown = rows();
    return m_state == CoverageState::Listed && m_focused < shown.size() ? &shown[m_focused] : nullptr;
}

std::vector<CoverageClip> CoverageModel::systemClips() const {
    std::vector<CoverageClip> clips;
    if (const CoverageRow* row = system()) {
        for (const CoverageClip& clip : m_content.clips) {
            if (clip.system == row->name) {
                clips.push_back(clip);
            }
        }
    }
    return clips;
}

std::string CoverageModel::rowElement(std::size_t index) {
    return fmt::format("cv-row-{}", index);
}

std::string CoverageModel::focusedElement() const {
    if (!m_open || m_view != CoverageView::Systems || system() == nullptr) {
        return {};
    }
    return rowElement(m_focused);
}

void CoverageModel::moveTo(std::size_t index) {
    const std::vector<CoverageRow>& shown = rows();
    if (shown.empty()) {
        return;
    }
    index = std::min(index, shown.size() - 1);
    if (index != m_focused || m_focusedName != shown[index].name) {
        m_focused = index;
        m_focusedName = shown[index].name;
        ++m_revision;
    }
}

void CoverageModel::step(int delta) {
    const std::ptrdiff_t next = static_cast<std::ptrdiff_t>(m_focused) + delta;
    moveTo(static_cast<std::size_t>(std::max<std::ptrdiff_t>(next, 0)));
}

void CoverageModel::setTab(CoverageTab tab) {
    if (tab == m_tab) {
        return;
    }
    m_tab = tab;
    // The same system stays focused when the other population lists it.
    const std::vector<CoverageRow>& shown = rows();
    const auto found = std::find_if(shown.begin(), shown.end(),
                                    [&](const CoverageRow& row) { return row.name == m_focusedName; });
    m_focused = found != shown.end() ? static_cast<std::size_t>(found - shown.begin()) : 0;
    if (found == shown.end()) {
        m_view = CoverageView::Systems;
        m_focusedName = shown.empty() ? std::string() : shown.front().name;
    }
    ++m_revision;
}

std::optional<CoverageCommand> CoverageModel::apply(UiCommand command) {
    if (!m_open) {
        return std::nullopt;
    }
    switch (command) {
    case UiCommand::Left:
    case UiCommand::FilterPrevious:
        setTab(CoverageTab::Games);
        return std::nullopt;
    case UiCommand::Right:
    case UiCommand::FilterNext:
        setTab(CoverageTab::Scraped);
        return std::nullopt;
    case UiCommand::Back:
        if (m_view == CoverageView::System) {
            m_view = CoverageView::Systems;
            ++m_revision;
            return std::nullopt;
        }
        close();
        return CoverageCommand::Close;
    default:
        break;
    }
    if (m_view != CoverageView::Systems || m_state != CoverageState::Listed) {
        return std::nullopt;
    }
    const int page = static_cast<int>(coverage::kPageRows);
    switch (command) {
    case UiCommand::Up:
        step(-1);
        break;
    case UiCommand::Down:
        step(1);
        break;
    case UiCommand::PageUp:
        step(-page);
        break;
    case UiCommand::PageDown:
        step(page);
        break;
    case UiCommand::Home:
        moveTo(0);
        break;
    case UiCommand::End:
        moveTo(rows().empty() ? 0 : rows().size() - 1);
        break;
    case UiCommand::Activate:
        if (system() != nullptr) {
            m_view = CoverageView::System;
            ++m_revision;
        }
        break;
    default:
        break;
    }
    return std::nullopt;
}

std::optional<CoverageCommand> CoverageModel::click(const CoverageClick& click) {
    if (!m_open) {
        return std::nullopt;
    }
    switch (click.kind) {
    case CoverageClick::Kind::Tab:
        setTab(click.tab);
        return std::nullopt;
    case CoverageClick::Kind::System:
        if (m_view == CoverageView::Systems && click.index >= 0 &&
            static_cast<std::size_t>(click.index) < rows().size()) {
            moveTo(static_cast<std::size_t>(click.index));
            return apply(UiCommand::Activate);
        }
        return std::nullopt;
    case CoverageClick::Kind::Close:
        return apply(UiCommand::Back);
    }
    return std::nullopt;
}

void CoverageModel::setContent(CoverageContent content) {
    m_state = content.empty ? CoverageState::Empty : CoverageState::Listed;
    m_content = std::move(content);
    const std::vector<CoverageRow>& shown = rows();
    const auto found = std::find_if(shown.begin(), shown.end(),
                                    [&](const CoverageRow& row) { return row.name == m_focusedName; });
    m_focused = found != shown.end() ? static_cast<std::size_t>(found - shown.begin()) : 0;
    m_focusedName = shown.empty() ? std::string() : shown[m_focused].name;
    if (found == shown.end()) {
        m_view = CoverageView::Systems;
    }
    ++m_revision;
}

void CoverageModel::setUnavailable() {
    m_state = CoverageState::Unavailable;
    m_content = {};
    m_view = CoverageView::Systems;
    m_focused = 0;
    m_focusedName.clear();
    ++m_revision;
}

} // namespace rv::ui
