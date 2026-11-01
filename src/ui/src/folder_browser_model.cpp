#include <rv/ui/folder_browser_model.hpp>

#include <algorithm>
#include <utility>

#include <fmt/format.h>

namespace rv::ui {

std::string_view toString(FolderBrowserState state) noexcept {
    switch (state) {
    case FolderBrowserState::Closed:
        return "closed";
    case FolderBrowserState::Loading:
        return "loading";
    case FolderBrowserState::Listed:
        return "listed";
    case FolderBrowserState::Failed:
        return "failed";
    }
    return "unknown";
}

std::string_view toString(FolderBrowserFailure failure) noexcept {
    switch (failure) {
    case FolderBrowserFailure::Missing:
        return "missing";
    case FolderBrowserFailure::Unreachable:
        return "unreachable";
    case FolderBrowserFailure::Unreadable:
        return "unreadable";
    }
    return "unknown";
}

void FolderBrowserModel::open(const std::optional<std::filesystem::path>& start) {
    go(start.value_or(std::filesystem::path{}));
}

void FolderBrowserModel::close() noexcept {
    m_state = FolderBrowserState::Closed;
    m_failure.reset();
    m_awaited.reset();
    m_requestPending = false;
    m_rows.clear();
    ++m_revision;
}

void FolderBrowserModel::go(std::filesystem::path folder) {
    m_folder = std::move(folder);
    m_state = FolderBrowserState::Loading;
    m_failure.reset();
    m_rows.clear();
    m_first = 0;
    m_selected = 0;
    m_focus = FolderBrowserFocus::Up;
    m_awaited = ++m_serial;
    m_requestPending = true;
    ++m_revision;
}

void FolderBrowserModel::goUp() {
    if (atDrives()) {
        return;
    }
    const std::filesystem::path parent = m_folder.parent_path();
    // A drive's root is its own parent: above it stand the drives.
    go(parent == m_folder ? std::filesystem::path{} : parent);
}

std::optional<FolderListingRequest> FolderBrowserModel::takeRequest() noexcept {
    if (!std::exchange(m_requestPending, false) || !m_awaited) {
        return std::nullopt;
    }
    return FolderListingRequest{*m_awaited, m_folder};
}

void FolderBrowserModel::setListing(std::uint64_t serial, std::vector<FolderRow> rows) {
    if (m_awaited != serial) {
        return;
    }
    m_awaited.reset();
    m_rows = std::move(rows);
    m_state = FolderBrowserState::Listed;
    m_focus = m_rows.empty() ? (canUse() ? FolderBrowserFocus::Use : FolderBrowserFocus::Up) : FolderBrowserFocus::Row;
    ++m_revision;
}

void FolderBrowserModel::setFailure(std::uint64_t serial, FolderBrowserFailure failure) {
    if (m_awaited != serial) {
        return;
    }
    m_awaited.reset();
    m_rows.clear();
    m_failure = failure;
    m_state = FolderBrowserState::Failed;
    m_focus = atDrives() ? FolderBrowserFocus::Cancel : FolderBrowserFocus::Up;
    ++m_revision;
}

void FolderBrowserModel::keepInView() noexcept {
    if (m_selected < m_first) {
        m_first = m_selected;
    } else if (m_selected >= m_first + folder_browser::kVisibleRows) {
        m_first = m_selected + 1 - folder_browser::kVisibleRows;
    }
}

void FolderBrowserModel::focusOn(FolderBrowserFocus focus) noexcept {
    if (m_focus != focus) {
        m_focus = focus;
        ++m_revision;
    }
}

void FolderBrowserModel::moveRow(std::ptrdiff_t delta) {
    if (m_rows.empty()) {
        return;
    }
    if (m_focus != FolderBrowserFocus::Row) {
        focusOn(FolderBrowserFocus::Row);
        return;
    }
    const auto last = static_cast<std::ptrdiff_t>(m_rows.size()) - 1;
    const std::ptrdiff_t wanted = static_cast<std::ptrdiff_t>(m_selected) + delta;
    if (wanted < 0 && m_selected == 0 && !atDrives()) {
        focusOn(FolderBrowserFocus::Up); // above the first row
        return;
    }
    const auto next = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(wanted, 0, last));
    if (next != m_selected) {
        m_selected = next;
        keepInView();
        ++m_revision;
    }
}

std::optional<FolderBrowserResult> FolderBrowserModel::choose() {
    if (!canUse()) {
        return std::nullopt;
    }
    FolderBrowserResult result{m_folder};
    close();
    return result;
}

std::optional<FolderBrowserResult> FolderBrowserModel::press() {
    switch (m_focus) {
    case FolderBrowserFocus::Up:
        goUp();
        return std::nullopt;
    case FolderBrowserFocus::Row:
        if (m_selected < m_rows.size()) {
            go(m_rows[m_selected].path);
        }
        return std::nullopt;
    case FolderBrowserFocus::Use:
        return choose();
    case FolderBrowserFocus::Cancel:
        close();
        return FolderBrowserResult{};
    }
    return std::nullopt;
}

std::optional<FolderBrowserResult> FolderBrowserModel::apply(UiCommand command) {
    if (!isOpen()) {
        return std::nullopt;
    }
    switch (command) {
    case UiCommand::Up:
        moveRow(-1);
        return std::nullopt;
    case UiCommand::Down:
        if (m_focus == FolderBrowserFocus::Up || m_focus == FolderBrowserFocus::Use ||
            m_focus == FolderBrowserFocus::Cancel) {
            moveRow(0);
        } else {
            moveRow(1);
        }
        return std::nullopt;
    case UiCommand::PageUp:
        moveRow(-static_cast<std::ptrdiff_t>(folder_browser::kVisibleRows));
        return std::nullopt;
    case UiCommand::PageDown:
        moveRow(static_cast<std::ptrdiff_t>(folder_browser::kVisibleRows));
        return std::nullopt;
    case UiCommand::Home:
        moveRow(-static_cast<std::ptrdiff_t>(m_rows.size()));
        return std::nullopt;
    case UiCommand::End:
        moveRow(static_cast<std::ptrdiff_t>(m_rows.size()));
        return std::nullopt;
    case UiCommand::Left:
    case UiCommand::Right: {
        // The rows (or the row up), Usa when it can choose, Annulla, round.
        const bool right = command == UiCommand::Right;
        const FolderBrowserFocus list = m_rows.empty() ? FolderBrowserFocus::Up : FolderBrowserFocus::Row;
        const FolderBrowserFocus use = canUse() ? FolderBrowserFocus::Use : FolderBrowserFocus::Cancel;
        if (m_focus == FolderBrowserFocus::Use) {
            focusOn(right ? FolderBrowserFocus::Cancel : list);
        } else if (m_focus == FolderBrowserFocus::Cancel) {
            focusOn(right ? list : use);
        } else {
            focusOn(right ? use : FolderBrowserFocus::Cancel);
        }
        return std::nullopt;
    }
    case UiCommand::Activate:
        return press();
    case UiCommand::Info:
        return choose();
    case UiCommand::Back:
        close();
        return FolderBrowserResult{};
    default:
        return std::nullopt;
    }
}

std::optional<FolderBrowserResult> FolderBrowserModel::click(const FolderBrowserClick& click) {
    if (!isOpen()) {
        return std::nullopt;
    }
    switch (click.kind) {
    case FolderBrowserClick::Kind::Up:
        m_focus = FolderBrowserFocus::Up;
        break;
    case FolderBrowserClick::Kind::Row:
        if (m_first + click.index >= m_rows.size()) {
            return std::nullopt;
        }
        m_focus = FolderBrowserFocus::Row;
        m_selected = m_first + click.index;
        break;
    case FolderBrowserClick::Kind::Use:
        if (!canUse()) {
            return std::nullopt; // the keys never reach it either
        }
        m_focus = FolderBrowserFocus::Use;
        break;
    case FolderBrowserClick::Kind::Cancel:
        m_focus = FolderBrowserFocus::Cancel;
        break;
    }
    ++m_revision;
    return press();
}

void FolderBrowserModel::scroll(int lines) {
    if (!isOpen() || lines == 0 || m_rows.size() <= folder_browser::kVisibleRows) {
        return;
    }
    const std::size_t last = m_rows.size() - folder_browser::kVisibleRows;
    const std::ptrdiff_t wanted = static_cast<std::ptrdiff_t>(m_first) + lines;
    const auto first = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(wanted, 0, static_cast<std::ptrdiff_t>(last)));
    if (first == m_first) {
        return;
    }
    m_first = first;
    // The focused row stays in view.
    m_selected = std::clamp(m_selected, m_first, m_first + folder_browser::kVisibleRows - 1);
    ++m_revision;
}

std::vector<FolderRow> FolderBrowserModel::rows() const {
    const std::size_t end = std::min(m_rows.size(), m_first + folder_browser::kVisibleRows);
    return {m_rows.begin() + static_cast<std::ptrdiff_t>(m_first), m_rows.begin() + static_cast<std::ptrdiff_t>(end)};
}

std::string FolderBrowserModel::rowElement(std::size_t index) {
    return fmt::format("fb-row-{}", index);
}

std::string FolderBrowserModel::focusedElement() const {
    switch (m_focus) {
    case FolderBrowserFocus::Up:
        return "fb-up";
    case FolderBrowserFocus::Row:
        return rowElement(m_selected - m_first);
    case FolderBrowserFocus::Use:
        return "fb-use";
    case FolderBrowserFocus::Cancel:
        return "fb-cancel";
    }
    return {};
}

} // namespace rv::ui
