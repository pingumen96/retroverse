#include <rv/ui/library_model.hpp>

#include <algorithm>
#include <utility>

#include <fmt/format.h>

namespace rv::ui {
namespace {

constexpr std::array<LibraryButton, 9> kButtons = {
    LibraryButton::Refresh,      LibraryButton::ApplyRemovals, LibraryButton::Hash,
    LibraryButton::KeepSeparate, LibraryButton::ReviewCancel,  LibraryButton::Pair,
    LibraryButton::PairCancel,   LibraryButton::Apply,         LibraryButton::RemovalsCancel};

/// The pass runs (or is being stopped).
[[nodiscard]] bool hashing(const LibraryLive& live) noexcept {
    return live.hash == "running" || live.hash == "cancelling";
}

} // namespace

std::string_view toString(LibraryState state) noexcept {
    switch (state) {
    case LibraryState::Loading:
        return "loading";
    case LibraryState::Listed:
        return "listed";
    case LibraryState::Unavailable:
        return "unavailable";
    }
    return "unknown";
}

std::string_view toString(LibraryTab tab) noexcept {
    switch (tab) {
    case LibraryTab::Sources:
        return "sources";
    case LibraryTab::Issues:
        return "issues";
    case LibraryTab::Removals:
        return "removals";
    case LibraryTab::Reviews:
        return "reviews";
    case LibraryTab::Hashes:
        return "hashes";
    }
    return "unknown";
}

std::optional<LibraryTab> libraryTabFromName(std::string_view name) noexcept {
    for (const LibraryTab tab : kLibraryTabs) {
        if (toString(tab) == name) {
            return tab;
        }
    }
    return std::nullopt;
}

std::string_view toString(LibraryView view) noexcept {
    switch (view) {
    case LibraryView::Main:
        return "main";
    case LibraryView::Review:
        return "review";
    case LibraryView::ConfirmPair:
        return "confirm-pair";
    case LibraryView::ConfirmRemovals:
        return "confirm-removals";
    }
    return "unknown";
}

std::string_view toString(LibraryCommandKind kind) noexcept {
    switch (kind) {
    case LibraryCommandKind::Close:
        return "close";
    case LibraryCommandKind::Load:
        return "load";
    case LibraryCommandKind::Refresh:
        return "refresh";
    case LibraryCommandKind::ApplyRemovals:
        return "apply-removals";
    case LibraryCommandKind::Resolve:
        return "resolve";
    case LibraryCommandKind::StartHash:
        return "start-hash";
    case LibraryCommandKind::CancelHash:
        return "cancel-hash";
    }
    return "unknown";
}

std::string_view toString(LibraryButton button) noexcept {
    switch (button) {
    case LibraryButton::Refresh:
        return "refresh";
    case LibraryButton::ApplyRemovals:
        return "apply-removals";
    case LibraryButton::Hash:
        return "hash";
    case LibraryButton::KeepSeparate:
        return "keep-separate";
    case LibraryButton::ReviewCancel:
        return "review-cancel";
    case LibraryButton::Pair:
        return "pair";
    case LibraryButton::PairCancel:
        return "pair-cancel";
    case LibraryButton::Apply:
        return "apply";
    case LibraryButton::RemovalsCancel:
        return "removals-cancel";
    }
    return "unknown";
}

std::optional<LibraryButton> libraryButtonFromName(std::string_view name) noexcept {
    for (const LibraryButton button : kButtons) {
        if (toString(button) == name) {
            return button;
        }
    }
    return std::nullopt;
}

std::string_view elementIdOf(LibraryButton button) noexcept {
    switch (button) {
    case LibraryButton::Refresh:
        return "lb-refresh";
    case LibraryButton::ApplyRemovals:
        return "lb-apply-removals";
    case LibraryButton::Hash:
        return "lb-hash";
    case LibraryButton::KeepSeparate:
        return "lb-keep-separate";
    case LibraryButton::ReviewCancel:
        return "lb-review-cancel";
    case LibraryButton::Pair:
        return "lb-pair";
    case LibraryButton::PairCancel:
        return "lb-pair-cancel";
    case LibraryButton::Apply:
        return "lb-apply";
    case LibraryButton::RemovalsCancel:
        return "lb-removals-cancel";
    }
    return {};
}

std::vector<LibraryRoot> withModes(std::vector<LibraryRoot> roots, const LibraryLive& live) {
    const auto modeOf = [&](std::string_view name) -> std::optional<std::string> {
        for (const auto& [root, mode] : live.modes) {
            if (root == name) {
                return mode;
            }
        }
        return std::nullopt;
    };
    const auto badness = [](std::string_view mode) {
        return mode == "lost" ? 4 : mode == "poll" ? 3 : mode == "arming" ? 2 : mode == "notify" ? 1 : 0;
    };
    for (LibraryRoot& root : roots) {
        root.mode = "off";
        if (!live.watching) {
            continue;
        }
        std::vector<std::string> names;
        if (root.kind == "rom" || root.kind == "media") {
            names.push_back(root.kind);
        } else if (root.kind == "esde-home") {
            names = {"gamelists", "settings", "custom-systems"};
        } else if (root.kind == "system") {
            names.push_back("system:" + root.system);
        }
        for (const std::string& name : names) {
            if (const std::optional<std::string> mode = modeOf(name); mode && badness(*mode) > badness(root.mode)) {
                root.mode = *mode;
            }
        }
    }
    return roots;
}

std::string LibraryModel::reviewElement(std::size_t index) {
    return fmt::format("lb-review-{}", index);
}

std::string LibraryModel::candidateElement(std::size_t index) {
    return fmt::format("lb-candidate-{}", index);
}

void LibraryModel::open() {
    m_open = true;
    m_state = LibraryState::Loading;
    m_view = LibraryView::Main;
    m_tab = LibraryTab::Sources;
    m_content = {};
    m_review = 0;
    m_candidate = 0;
    m_resolving = false;
    m_problem.clear();
    m_request = LibraryCommand{LibraryCommandKind::Load, {}, 0, {}};
    focusFirst();
    ++m_revision;
}

void LibraryModel::close() noexcept {
    if (m_open) {
        m_open = false;
        ++m_revision;
    }
}

std::optional<LibraryCommand> LibraryModel::takeRequest() noexcept {
    return std::exchange(m_request, std::nullopt);
}

const LibraryReview* LibraryModel::review() const noexcept {
    for (const LibraryReview& row : m_content.reviews) {
        if (row.id == m_review) {
            return &row;
        }
    }
    return nullptr;
}

const LibraryCandidate* LibraryModel::candidate() const noexcept {
    const LibraryReview* row = review();
    return row != nullptr && m_candidate < row->candidates.size() ? &row->candidates[m_candidate] : nullptr;
}

std::vector<LibraryModel::Item> LibraryModel::items() const {
    std::vector<Item> shown;
    switch (m_view) {
    case LibraryView::Main:
        if (m_state != LibraryState::Listed) {
            break;
        }
        switch (m_tab) {
        case LibraryTab::Sources:
            if (m_live.canRefresh) {
                shown.push_back(Item{LibraryButton::Refresh, 0});
            }
            break;
        case LibraryTab::Issues:
            break;
        case LibraryTab::Removals:
            if (!m_content.held.empty()) {
                shown.push_back(Item{LibraryButton::ApplyRemovals, 0});
            }
            break;
        case LibraryTab::Reviews:
            for (std::size_t index = 0; index < m_content.reviews.size(); ++index) {
                shown.push_back(Item{std::nullopt, index});
            }
            break;
        case LibraryTab::Hashes:
            if (m_content.files > 0 || hashing(m_live)) {
                shown.push_back(Item{LibraryButton::Hash, 0});
            }
            break;
        }
        break;
    case LibraryView::Review:
        // Only the review's own candidates that resolveReview() can take: those with an identity present now.
        if (const LibraryReview* row = review()) {
            for (std::size_t index = 0; index < row->candidates.size(); ++index) {
                if (!row->candidates[index].gameId.empty()) {
                    shown.push_back(Item{std::nullopt, index});
                }
            }
        }
        shown.push_back(Item{LibraryButton::KeepSeparate, 0});
        shown.push_back(Item{LibraryButton::ReviewCancel, 0});
        break;
    case LibraryView::ConfirmPair:
        shown.push_back(Item{LibraryButton::PairCancel, 0});
        shown.push_back(Item{LibraryButton::Pair, 0});
        break;
    case LibraryView::ConfirmRemovals:
        shown.push_back(Item{LibraryButton::RemovalsCancel, 0});
        shown.push_back(Item{LibraryButton::Apply, 0});
        break;
    }
    return shown;
}

bool LibraryModel::scrollsBody() const {
    return m_view == LibraryView::Main && (m_tab == LibraryTab::Issues || items().empty());
}

std::string LibraryModel::focusedElement() const {
    if (!m_open) {
        return {};
    }
    const std::vector<Item> shown = items();
    if (std::find(shown.begin(), shown.end(), m_focus) == shown.end()) {
        return {};
    }
    if (m_focus.button) {
        return std::string(elementIdOf(*m_focus.button));
    }
    return m_view == LibraryView::Review ? candidateElement(m_focus.index) : reviewElement(m_focus.index);
}

void LibraryModel::focusFirst() {
    const std::vector<Item> shown = items();
    m_focus = shown.empty() ? Item{} : shown.front();
}

void LibraryModel::move(int delta) {
    const std::vector<Item> shown = items();
    if (shown.empty()) {
        return;
    }
    const auto at = std::find(shown.begin(), shown.end(), m_focus);
    const std::ptrdiff_t from = at == shown.end() ? 0 : at - shown.begin();
    const std::ptrdiff_t to =
        std::clamp<std::ptrdiff_t>(from + delta, 0, static_cast<std::ptrdiff_t>(shown.size()) - 1);
    if (at == shown.end() || to != from) {
        m_focus = shown[static_cast<std::size_t>(to)];
        ++m_revision;
    }
}

void LibraryModel::show(LibraryView view) {
    m_view = view;
    focusFirst();
    ++m_revision;
}

void LibraryModel::setTab(LibraryTab tab) {
    if (m_view != LibraryView::Main || tab == m_tab) {
        return;
    }
    m_tab = tab;
    focusFirst();
    ++m_revision;
}

std::optional<LibraryCommand> LibraryModel::press(const Item& item) {
    if (!item.button) {
        if (m_view == LibraryView::Main && item.index < m_content.reviews.size()) {
            m_review = m_content.reviews[item.index].id;
            m_problem.clear();
            show(LibraryView::Review);
        } else if (m_view == LibraryView::Review) {
            const LibraryReview* row = review();
            if (row != nullptr && item.index < row->candidates.size() && !row->candidates[item.index].gameId.empty()) {
                m_candidate = item.index;
                show(LibraryView::ConfirmPair); // pairing cannot be undone: asked first, Annulla focused
            }
        }
        return std::nullopt;
    }
    switch (*item.button) {
    case LibraryButton::Refresh:
        if (!m_live.canRefresh || m_live.refreshing) {
            return std::nullopt;
        }
        return LibraryCommand{LibraryCommandKind::Refresh, {}, 0, {}};
    case LibraryButton::ApplyRemovals:
        if (m_content.held.empty()) {
            return std::nullopt;
        }
        show(LibraryView::ConfirmRemovals); // nothing is removed before Applica: Annulla focused
        return std::nullopt;
    case LibraryButton::Hash:
        if (hashing(m_live)) {
            return m_live.hash == "running" ? std::optional<LibraryCommand>(
                                                  LibraryCommand{LibraryCommandKind::CancelHash, {}, 0, {}})
                                            : std::nullopt;
        }
        return LibraryCommand{LibraryCommandKind::StartHash, {}, 0, {}};
    case LibraryButton::KeepSeparate:
        if (const LibraryReview* row = review(); row != nullptr && !m_resolving) {
            m_resolving = true;
            m_problem.clear();
            ++m_revision;
            return LibraryCommand{LibraryCommandKind::Resolve, {}, row->id, {}};
        }
        return std::nullopt;
    case LibraryButton::ReviewCancel:
        m_problem.clear();
        show(LibraryView::Main);
        m_focus = Item{std::nullopt, 0};
        for (std::size_t index = 0; index < m_content.reviews.size(); ++index) {
            if (m_content.reviews[index].id == m_review) {
                m_focus = Item{std::nullopt, index};
            }
        }
        return std::nullopt;
    case LibraryButton::Pair: {
        const LibraryReview* row = review();
        const LibraryCandidate* chosen = candidate();
        if (row == nullptr || chosen == nullptr || chosen->gameId.empty() || m_resolving) {
            return std::nullopt;
        }
        m_resolving = true;
        m_problem.clear();
        const LibraryCommand command{LibraryCommandKind::Resolve, {}, row->id, chosen->gameId};
        show(LibraryView::Review);
        return command;
    }
    case LibraryButton::PairCancel:
        show(LibraryView::Review);
        return std::nullopt;
    case LibraryButton::Apply: {
        LibraryCommand command{LibraryCommandKind::ApplyRemovals, {}, 0, {}};
        for (const LibraryHeld& held : m_content.held) {
            command.systems.push_back(held.name);
        }
        show(LibraryView::Main);
        return command;
    }
    case LibraryButton::RemovalsCancel:
        show(LibraryView::Main);
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<LibraryCommand> LibraryModel::apply(UiCommand command) {
    if (!m_open) {
        return std::nullopt;
    }
    const int page = static_cast<int>(library::kPageRows);
    switch (command) {
    case UiCommand::Left:
    case UiCommand::FilterPrevious:
        if (m_view == LibraryView::Main && m_tab != kLibraryTabs.front()) {
            setTab(static_cast<LibraryTab>(static_cast<int>(m_tab) - 1));
        }
        return std::nullopt;
    case UiCommand::Right:
    case UiCommand::FilterNext:
        if (m_view == LibraryView::Main && m_tab != kLibraryTabs.back()) {
            setTab(static_cast<LibraryTab>(static_cast<int>(m_tab) + 1));
        }
        return std::nullopt;
    case UiCommand::Up:
        move(-1);
        return std::nullopt;
    case UiCommand::Down:
        move(1);
        return std::nullopt;
    case UiCommand::PageUp:
        move(-page);
        return std::nullopt;
    case UiCommand::PageDown:
        move(page);
        return std::nullopt;
    case UiCommand::Home:
        move(-1'000'000);
        return std::nullopt;
    case UiCommand::End:
        move(1'000'000);
        return std::nullopt;
    case UiCommand::Activate: {
        const std::vector<Item> shown = items();
        if (std::find(shown.begin(), shown.end(), m_focus) == shown.end()) {
            return std::nullopt;
        }
        return press(m_focus);
    }
    case UiCommand::Back:
        switch (m_view) {
        case LibraryView::Main:
            close();
            return LibraryCommand{LibraryCommandKind::Close, {}, 0, {}};
        case LibraryView::Review:
            return press(Item{LibraryButton::ReviewCancel, 0});
        case LibraryView::ConfirmPair:
            show(LibraryView::Review);
            return std::nullopt;
        case LibraryView::ConfirmRemovals:
            show(LibraryView::Main);
            m_focus = Item{LibraryButton::ApplyRemovals, 0};
            return std::nullopt;
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

std::optional<LibraryCommand> LibraryModel::click(const LibraryClick& click) {
    if (!m_open) {
        return std::nullopt;
    }
    switch (click.kind) {
    case LibraryClick::Kind::Tab:
        setTab(click.tab);
        return std::nullopt;
    case LibraryClick::Kind::Close:
        if (m_view != LibraryView::Main) {
            return std::nullopt;
        }
        return apply(UiCommand::Back);
    case LibraryClick::Kind::Button:
    case LibraryClick::Kind::Review:
    case LibraryClick::Kind::Candidate: {
        const Item item = click.kind == LibraryClick::Kind::Button
                              ? Item{click.button, 0}
                              : Item{std::nullopt, static_cast<std::size_t>(std::max(click.index, 0))};
        const bool rowOfView = click.kind == LibraryClick::Kind::Button ||
                               (click.kind == LibraryClick::Kind::Review) == (m_view == LibraryView::Main);
        const std::vector<Item> shown = items();
        if (!rowOfView || std::find(shown.begin(), shown.end(), item) == shown.end()) {
            return std::nullopt;
        }
        if (m_focus != item) {
            m_focus = item;
            ++m_revision;
        }
        return press(item);
    }
    }
    return std::nullopt;
}

void LibraryModel::setContent(LibraryContent content) {
    m_state = LibraryState::Listed;
    m_content = std::move(content);
    if (m_view != LibraryView::Main && review() == nullptr) {
        m_view = LibraryView::Main; // the review went (resolved, its file back)
    }
    if (m_view == LibraryView::ConfirmRemovals && m_content.held.empty()) {
        m_view = LibraryView::Main;
    }
    const std::vector<Item> shown = items();
    if (std::find(shown.begin(), shown.end(), m_focus) == shown.end()) {
        focusFirst();
    }
    ++m_revision;
}

void LibraryModel::setUnavailable() {
    m_state = LibraryState::Unavailable;
    m_content = {};
    m_view = LibraryView::Main;
    focusFirst();
    ++m_revision;
}

void LibraryModel::setLive(LibraryLive live) {
    if (live == m_live) {
        return;
    }
    const bool refreshEnded = m_live.refreshing && !live.refreshing;
    const bool hashEnded = hashing(m_live) && !hashing(live);
    m_live = std::move(live);
    if (m_open && m_state != LibraryState::Unavailable && (refreshEnded || hashEnded)) {
        prepare(); // what the catalog holds changed
    }
    const std::vector<Item> shown = items();
    if (std::find(shown.begin(), shown.end(), m_focus) == shown.end()) {
        focusFirst();
    }
    ++m_revision;
}

void LibraryModel::resolved(std::string_view problem) {
    m_resolving = false;
    m_problem = std::string(problem);
    if (m_problem.empty()) {
        m_view = LibraryView::Main;
        m_tab = LibraryTab::Reviews;
        focusFirst();
        prepare();
    }
    ++m_revision;
}

} // namespace rv::ui
