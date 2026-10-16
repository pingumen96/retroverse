#include <rv/ui/quick_catalog_model.hpp>

#include <algorithm>
#include <utility>

namespace rv::ui {
namespace {

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

} // namespace

QuickCatalogModel::QuickCatalogModel(CatalogSource& source, std::size_t windowRows, std::size_t scrollMargin)
    : m_source(source), m_filters(1), m_windowRows(std::max<std::size_t>(windowRows, 1)), m_scrollMargin(scrollMargin),
      m_selected(kNone) {
    readFilters(); // the chip of every game stays when the platforms cannot be read
    materialize(Restore::FirstEntry);
}

void QuickCatalogModel::fail(Error error) {
    if (!m_error) {
        m_error = std::move(error); // the first failure of the operation: the later ones follow from it
    }
}

void QuickCatalogModel::readFilters() {
    Expected<std::vector<PlatformFilter>> platforms = m_source.platforms();
    if (!platforms) {
        // The chips and the active filter stay as they were: the catalog did not lose its platforms, it could not be
        // read (the query that follows tells the list's fate).
        fail(std::move(platforms.error()));
        return;
    }
    std::vector<PlatformFilter> filters(1); // every game: the empty key, the chip the documents name
    for (PlatformFilter& platform : *platforms) {
        if (platform.key.empty()) {
            continue; // the empty key is every game's
        }
        filters.front().count += platform.count;
        filters.push_back(std::move(platform));
    }
    if (filters != m_filters) {
        m_filters = std::move(filters);
        ++m_filterRevision;
    }
    const bool known = std::any_of(m_filters.begin(), m_filters.end(),
                                   [this](const PlatformFilter& filter) { return filter.key == m_activeFilter; });
    if (!known) {
        m_activeFilter.clear(); // its platform is gone: every game
        ++m_filterRevision;
    }
}

bool QuickCatalogModel::setFilter(std::string_view key) {
    const bool known = std::any_of(m_filters.begin(), m_filters.end(),
                                   [key](const PlatformFilter& filter) { return filter.key == key; });
    if (!known) {
        return false;
    }
    if (m_activeFilter != key) {
        m_error.reset();
        m_activeFilter = std::string(key);
        ++m_filterRevision;
        materialize(Restore::FirstEntry);
    }
    return true;
}

bool QuickCatalogModel::cycleFilter(int step) {
    const auto size = static_cast<std::ptrdiff_t>(m_filters.size());
    if (size < 2 || step == 0) {
        return false;
    }
    const auto active = std::find_if(m_filters.begin(), m_filters.end(),
                                     [this](const PlatformFilter& filter) { return filter.key == m_activeFilter; });
    const std::ptrdiff_t from = active != m_filters.end() ? active - m_filters.begin() : 0;
    std::ptrdiff_t to = (from + step % size) % size;
    if (to < 0) {
        to += size;
    }
    if (to == from) {
        return false;
    }
    return setFilter(m_filters[static_cast<std::size_t>(to)].key);
}

void QuickCatalogModel::setSearch(std::string_view text) {
    if (m_search == text) {
        return;
    }
    m_error.reset();
    m_search = std::string(text);
    materialize(Restore::FirstEntry);
}

void QuickCatalogModel::refresh() {
    m_error.reset();
    readFilters();
    materialize(Restore::NearestPosition);
}

void QuickCatalogModel::materialize(Restore restore) {
    Expected<QueryState> asked = m_source.query(CatalogSourceQuery{m_search, m_activeFilter});
    if (asked && *asked == QueryState::Pending) {
        // The source materializes on its worker: the list shown stays until poll() collects the new one. A search or
        // a filter asked since the last list decides how the selection is restored.
        m_pending = m_pending == Restore::FirstEntry ? Restore::FirstEntry : restore;
        return;
    }
    m_pending.reset();
    applyList(asked ? ok() : Status(propagate(std::move(asked.error()))), restore);
}

bool QuickCatalogModel::poll() {
    if (!m_pending) {
        return false;
    }
    Expected<QueryState> state = m_source.poll();
    if (state && *state == QueryState::Pending) {
        return false;
    }
    const Restore restore = *m_pending;
    m_pending.reset();
    m_error.reset(); // the operation that reads the list is this one
    applyList(state ? ok() : Status(propagate(std::move(state.error()))), restore);
    return true;
}

void QuickCatalogModel::applyList(const Status& listed, Restore restore) {
    const std::string previousId = m_selectedId;
    const std::size_t previousPosition = m_selected;
    const std::size_t previousSlot = m_selected != kNone ? m_selected - m_first : 0;
    if (listed) {
        m_count = m_source.count();
    } else {
        m_count = 0;
        fail(listed.error());
    }
    ++m_listRevision;

    std::size_t target = kNone;
    if (m_count > 0) {
        if (!previousId.empty()) {
            if (const std::optional<std::size_t> found = m_source.position(previousId); found && *found < m_count) {
                target = *found;
            }
        }
        if (target == kNone) {
            target = restore == Restore::NearestPosition && previousPosition != kNone
                         ? std::min(previousPosition, m_count - 1)
                         : 0;
        }
    }
    show(target, target != kNone ? placeFirst(target, previousSlot) : 0, Fetch::All);
}

void QuickCatalogModel::show(std::size_t position, std::size_t first, Fetch fetch) {
    const bool unread = m_count > 0 && m_window.empty(); // a read that failed: the next move reads the window again
    if (fetch == Fetch::All || first != m_first || unread) {
        if (fetch == Fetch::Moved) {
            m_error.reset(); // a move that reads the source: its outcome is the error now
        }
        readWindow(first, fetch == Fetch::All || unread);
        ++m_windowRevision;
    }
    m_selected = position;
    const CatalogEntry* row = selected();
    const std::string_view id = row != nullptr ? std::string_view(row->id) : std::string_view();
    if (id != m_selectedId) {
        m_selectedId.assign(id); // keeps its storage: no allocation per move once it holds an id
        ++m_selectionRevision;
    }
}

void QuickCatalogModel::readWindow(std::size_t first, bool whole) {
    const std::size_t previousFirst = m_first;
    const std::size_t previousEnd = m_first + m_window.size();
    m_first = first;
    if (m_count == 0) {
        m_window.clear();
        return;
    }
    const std::size_t size = std::min(m_windowRows, m_count - first);
    const std::size_t keptBegin = std::max(first, previousFirst);
    const std::size_t keptEnd = std::min(first + size, previousEnd);
    if (!whole && !m_window.empty() && keptBegin < keptEnd) {
        // A scroll: the rows the window keeps move to their new places (their strings swapped, not copied), only the
        // rows that enter it are read: one per step of a held direction instead of a page.
        m_spare.resize(size);
        for (std::size_t position = keptBegin; position < keptEnd; ++position) {
            std::swap(m_spare[position - first], m_window[position - previousFirst]);
        }
        if (readInto(first, keptBegin - first, 0) && readInto(keptEnd, first + size - keptEnd, keptEnd - first)) {
            std::swap(m_window, m_spare);
            return;
        }
        // The source failed or gave fewer rows than its count: the whole window, whose read decides.
    }
    if (Status read = m_source.rows(first, size, m_window); !read) {
        m_window.clear();
        fail(std::move(read.error()));
    }
}

bool QuickCatalogModel::readInto(std::size_t position, std::size_t count, std::size_t at) {
    if (count == 0) {
        return true;
    }
    if (Status read = m_source.rows(position, count, m_fetched); !read || m_fetched.size() != count) {
        return false;
    }
    for (std::size_t row = 0; row < count; ++row) {
        std::swap(m_spare[at + row], m_fetched[row]);
    }
    return true;
}

std::size_t QuickCatalogModel::maxFirst() const noexcept {
    return m_count > m_windowRows ? m_count - m_windowRows : 0;
}

std::size_t QuickCatalogModel::scrollMargin() const noexcept {
    return std::min(m_scrollMargin, (m_windowRows - 1) / 2);
}

std::size_t QuickCatalogModel::placeFirst(std::size_t position, std::size_t slot) const noexcept {
    const std::size_t margin = scrollMargin();
    const std::size_t row = std::clamp(slot, margin, m_windowRows - 1 - margin);
    return std::min(position > row ? position - row : 0, maxFirst());
}

std::size_t QuickCatalogModel::followFirst(std::size_t position, std::size_t first, bool down) const noexcept {
    const std::size_t margin = scrollMargin();
    if (down) {
        const std::size_t lowestRow = m_windowRows - 1 - margin; // the selection no lower than this row
        first = std::max(first, position > lowestRow ? position - lowestRow : 0);
    } else {
        first = std::min(first, position > margin ? position - margin : 0);
    }
    return std::min(first, maxFirst());
}

void QuickCatalogModel::setWindowRows(std::size_t rows) {
    rows = std::max<std::size_t>(rows, 1);
    if (rows == m_windowRows) {
        return;
    }
    m_error.reset();
    const std::size_t slot = m_selected != kNone ? m_selected - m_first : 0;
    m_windowRows = rows;
    show(m_selected, m_selected != kNone ? placeFirst(m_selected, slot) : 0, Fetch::All);
}

std::optional<std::size_t> QuickCatalogModel::selectedPosition() const noexcept {
    return m_selected != kNone ? std::optional<std::size_t>(m_selected) : std::nullopt;
}

std::optional<std::size_t> QuickCatalogModel::selectedSlot() const noexcept {
    return m_selected != kNone ? std::optional<std::size_t>(m_selected - m_first) : std::nullopt;
}

const CatalogEntry* QuickCatalogModel::selected() const noexcept {
    if (m_selected == kNone || m_selected - m_first >= m_window.size()) {
        return nullptr;
    }
    return &m_window[m_selected - m_first];
}

bool QuickCatalogModel::select(std::string_view id) {
    const std::optional<std::size_t> position = m_source.position(id);
    return position && selectPosition(*position);
}

bool QuickCatalogModel::selectPosition(std::size_t position) {
    if (position >= m_count) {
        return false;
    }
    if (position == m_selected) {
        return true;
    }
    const bool shown = position >= m_first && position - m_first < m_windowRows;
    const bool down = m_selected == kNone || position > m_selected;
    show(position, shown ? m_first : followFirst(position, m_first, down), Fetch::Moved);
    return true;
}

bool QuickCatalogModel::moveSelection(std::ptrdiff_t delta) {
    if (m_selected == kNone || delta == 0) {
        return false;
    }
    const std::size_t last = m_count - 1;
    std::size_t target = m_selected;
    if (delta < 0) {
        // The magnitude in unsigned arithmetic: no overflow for the most negative delta.
        const std::size_t up = std::size_t{0} - static_cast<std::size_t>(delta);
        target = m_selected > up ? m_selected - up : 0;
    } else {
        const auto down = static_cast<std::size_t>(delta);
        target = last - m_selected > down ? m_selected + down : last;
    }
    if (target == m_selected) {
        return false;
    }
    show(target, followFirst(target, m_first, target > m_selected), Fetch::Moved);
    return true;
}

bool QuickCatalogModel::pageUp() {
    if (m_selected == kNone) {
        return false;
    }
    // The selection and the window move together, so the selection keeps its row (both stop at the list's start).
    const std::size_t page = m_windowRows;
    const std::size_t target = m_selected > page ? m_selected - page : 0;
    const std::size_t first = m_first > page ? m_first - page : 0;
    if (target == m_selected && first == m_first) {
        return false;
    }
    show(target, first, Fetch::Moved);
    return true;
}

bool QuickCatalogModel::pageDown() {
    if (m_selected == kNone) {
        return false;
    }
    // As pageUp(): at the list's end the window stops at the last page and the selection at the last game.
    const std::size_t page = m_windowRows;
    const std::size_t last = m_count - 1;
    const std::size_t target = last - m_selected > page ? m_selected + page : last;
    const std::size_t first = std::min(m_first + page, maxFirst());
    if (target == m_selected && first == m_first) {
        return false;
    }
    show(target, first, Fetch::Moved);
    return true;
}

bool QuickCatalogModel::home() {
    if (m_selected == kNone || (m_selected == 0 && m_first == 0)) {
        return false;
    }
    show(0, 0, Fetch::Moved);
    return true;
}

bool QuickCatalogModel::end() {
    if (m_selected == kNone) {
        return false;
    }
    const std::size_t last = m_count - 1;
    if (m_selected == last && m_first == maxFirst()) {
        return false;
    }
    show(last, maxFirst(), Fetch::Moved);
    return true;
}

} // namespace rv::ui
