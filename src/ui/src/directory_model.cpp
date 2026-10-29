#include <rv/ui/directory_model.hpp>

#include <algorithm>
#include <utility>

#include <rv/ui/quick_catalog_room.hpp>

namespace rv::ui {
namespace {

/// The card's buttons in focus order.
constexpr std::array<DirectoryCardButton, 3> kDirectoryCardButtons{
    DirectoryCardButton::Play, DirectoryCardButton::ShowInRoom, DirectoryCardButton::Back};

constexpr std::array<DirectoryTab, 3> kDirectoryTabs{DirectoryTab::Floors, DirectoryTab::Games,
                                                     DirectoryTab::Changes};

/// The tab after `tab` (`forward`) or before it, round the bar (the shoulders), or stopping at its ends (the tab bar's
/// Left and Right).
[[nodiscard]] DirectoryTab nextTab(DirectoryTab tab, bool forward, bool wrap) noexcept {
    const auto index = static_cast<std::size_t>(tab);
    const std::size_t count = kDirectoryTabs.size();
    if (forward) {
        return index + 1 < count ? kDirectoryTabs[index + 1] : (wrap ? kDirectoryTabs.front() : tab);
    }
    return index > 0 ? kDirectoryTabs[index - 1] : (wrap ? kDirectoryTabs.back() : tab);
}

} // namespace

std::string_view toString(DirectoryTab tab) noexcept {
    switch (tab) {
    case DirectoryTab::Floors:
        return "floors";
    case DirectoryTab::Games:
        return "games";
    case DirectoryTab::Changes:
        return "changes";
    }
    return "unknown";
}

std::optional<DirectoryTab> directoryTabFromName(std::string_view name) noexcept {
    for (const DirectoryTab tab : kDirectoryTabs) {
        if (toString(tab) == name) {
            return tab;
        }
    }
    return std::nullopt;
}

std::string_view toString(DirectoryView view) noexcept {
    switch (view) {
    case DirectoryView::Floors:
        return "floors";
    case DirectoryView::Rooms:
        return "rooms";
    case DirectoryView::Games:
        return "games";
    case DirectoryView::Card:
        return "card";
    case DirectoryView::Changes:
        return "changes";
    }
    return "unknown";
}

std::string_view toString(DirectoryAction action) noexcept {
    switch (action) {
    case DirectoryAction::EditRoom:
        return directory::kActions[0];
    case DirectoryAction::RegenerateRoom:
        return directory::kActions[1];
    case DirectoryAction::RegenerateFloor:
        return directory::kActions[2];
    case DirectoryAction::RegenerateWorld:
        return directory::kActions[3];
    case DirectoryAction::Undo:
        return directory::kActions[4];
    }
    return "unknown";
}

std::string_view toString(DirectoryGamesState state) noexcept {
    switch (state) {
    case DirectoryGamesState::Loading:
        return directory::kGamesStates[0];
    case DirectoryGamesState::Ready:
        return directory::kGamesStates[1];
    case DirectoryGamesState::Failed:
        return directory::kGamesStates[2];
    }
    return "unknown";
}

std::string_view toString(DirectoryCardButton button) noexcept {
    switch (button) {
    case DirectoryCardButton::Play:
        return "play";
    case DirectoryCardButton::ShowInRoom:
        return "room";
    case DirectoryCardButton::Back:
        return "back";
    }
    return "unknown";
}

std::optional<DirectoryCardButton> directoryCardButtonFromName(std::string_view name) noexcept {
    for (const DirectoryCardButton button : kDirectoryCardButtons) {
        if (toString(button) == name) {
            return button;
        }
    }
    return std::nullopt;
}

std::string_view elementIdOf(DirectoryCardButton button) noexcept {
    switch (button) {
    case DirectoryCardButton::Play:
        return directory::kCardPlayId;
    case DirectoryCardButton::ShowInRoom:
        return directory::kCardRoomId;
    case DirectoryCardButton::Back:
        return directory::kCardBackId;
    }
    return directory::kCardPlayId;
}

std::string_view toString(DirectoryCommandKind kind) noexcept {
    switch (kind) {
    case DirectoryCommandKind::ChooseFloor:
        return "choose-floor";
    case DirectoryCommandKind::ChooseRoom:
        return "choose-room";
    case DirectoryCommandKind::Launch:
        return "launch";
    case DirectoryCommandKind::ShowInRoom:
        return "show-in-room";
    case DirectoryCommandKind::Close:
        return "close";
    case DirectoryCommandKind::EditRoom:
        return "edit-room";
    case DirectoryCommandKind::RegenerateRoom:
        return "regenerate-room";
    case DirectoryCommandKind::RegenerateFloor:
        return "regenerate-floor";
    case DirectoryCommandKind::RegenerateWorld:
        return "regenerate-world";
    case DirectoryCommandKind::Undo:
        return "undo";
    }
    return "unknown";
}

bool DirectoryModel::editShown() const noexcept {
    return m_view == DirectoryView::Floors && !m_building.editableRoom.empty();
}

std::vector<DirectoryAction> DirectoryModel::actions() const {
    std::vector<DirectoryAction> shown;
    if (!m_building.editableRoom.empty()) {
        shown.push_back(DirectoryAction::EditRoom);
    }
    shown.insert(shown.end(), {DirectoryAction::RegenerateRoom, DirectoryAction::RegenerateFloor,
                               DirectoryAction::RegenerateWorld, DirectoryAction::Undo});
    return shown;
}

bool DirectoryModel::actionEnabled(DirectoryAction action) const noexcept {
    switch (action) {
    case DirectoryAction::EditRoom:
        return !m_building.editableRoom.empty();
    case DirectoryAction::RegenerateRoom:
        return !m_building.regenerableRoom.empty();
    case DirectoryAction::RegenerateFloor:
        return !m_building.currentFloor.empty();
    case DirectoryAction::RegenerateWorld:
    case DirectoryAction::Undo:
        return true;
    }
    return false;
}

void DirectoryModel::setBuilding(DirectoryBuilding building) {
    const std::vector<DirectoryFloor>& floors = m_building.floors;
    const std::string selected = m_floor < floors.size() ? floors[m_floor].id : std::string();
    const std::string opened = m_openedFloor < floors.size() ? floors[m_openedFloor].id : std::string();
    std::string room;
    if (m_openedFloor < floors.size() && m_roomRow > 0 && m_roomRow <= floors[m_openedFloor].rooms.size()) {
        room = floors[m_openedFloor].rooms[m_roomRow - 1].id;
    }
    m_building = std::move(building);
    const std::size_t count = m_building.floors.size();
    const std::size_t found = floorIndexOf(selected);
    m_floor = found < count ? found : (count == 0 ? 0 : std::min(m_floor, count - 1));
    const std::size_t openedIndex = floorIndexOf(opened);
    if (openedIndex < count) {
        m_openedFloor = openedIndex;
        const std::vector<DirectoryRoom>& rooms = m_building.floors[openedIndex].rooms;
        if (m_roomRow > 0) {
            const auto it = std::find_if(rooms.begin(), rooms.end(),
                                         [&](const DirectoryRoom& candidate) { return candidate.id == room; });
            m_roomRow = it != rooms.end() ? static_cast<std::size_t>(it - rooms.begin()) + 1
                                          : std::min(m_roomRow, rooms.size());
        }
    } else if (m_floorsView == DirectoryView::Rooms) {
        // The floor whose rooms were listed is gone: back to the floors.
        m_floorsView = DirectoryView::Floors;
        if (m_view == DirectoryView::Rooms) {
            m_view = DirectoryView::Floors;
        }
    }
    // I4b step 6: the "Modifiche" tab's row stays within its rows ("Modifica stanza" comes and goes above the others).
    if (const std::size_t rows = actions().size(); m_action >= rows) {
        m_action = rows - 1;
    }
    followRows();
    changed();
}

void DirectoryModel::setGames(std::vector<DirectoryGame> games) {
    const std::string selected = m_game < m_games.size() ? m_games[m_game].gameId : std::string();
    m_games = std::move(games);
    m_gamesState = DirectoryGamesState::Ready;
    const auto it = std::find_if(m_games.begin(), m_games.end(),
                                 [&](const DirectoryGame& game) { return game.gameId == selected; });
    if (m_gamesFromTop) {
        // The panel opened on the list's first game, the user has not moved since: the new list's first one.
        m_game = 0;
    } else if (it != m_games.end()) {
        m_game = static_cast<std::size_t>(it - m_games.begin());
    } else {
        m_game = m_games.empty() ? 0 : std::min(m_game, m_games.size() - 1);
    }
    followRows();
    changed();
}

void DirectoryModel::setGamesState(DirectoryGamesState state) {
    if (state == m_gamesState) {
        return;
    }
    m_gamesState = state;
    followRows();
    changed();
}

void DirectoryModel::setCard(GameCardContent content, std::string room) {
    if (m_view != DirectoryView::Card || content.gameId != m_cardGameId) {
        return;
    }
    m_cardContent = std::move(content);
    m_cardRoom = std::move(room);
    if (m_cardButton == DirectoryCardButton::ShowInRoom && !cardShowsRoom()) {
        m_cardButton = DirectoryCardButton::Play;
    }
    changed();
}

void DirectoryModel::setNotice(std::string key) {
    if (key == m_notice) {
        return;
    }
    m_notice = std::move(key);
    changed();
}

void DirectoryModel::open(DirectoryTab tab) {
    m_open = true;
    m_opened = true;
    m_notice.clear();
    m_tabsFocused = false;
    m_tabsForEmpty = false;
    m_editFocused = false;
    m_tab = tab;
    m_cardGameId.clear();
    m_closedCardGameId.clear();
    m_cardRoom.clear();
    m_cardButton = DirectoryCardButton::Play;
    // Both tabs from their start: the walker's floor, the first game.
    m_floorsView = DirectoryView::Floors;
    const std::size_t current = floorIndexOf(m_building.currentFloor);
    m_floor = current < m_building.floors.size() ? current : 0;
    m_game = 0;
    m_gamesFromTop = true;
    m_action = 0;
    if (tab == DirectoryTab::Floors) {
        m_view = DirectoryView::Floors;
    } else if (tab == DirectoryTab::Changes) {
        m_view = DirectoryView::Changes;
    } else {
        m_view = DirectoryView::Games;
        ++m_gamesRequest;
    }
    followRows();
    changed();
}

void DirectoryModel::openGame(const DirectoryGame& game) {
    open(DirectoryTab::Games);
    // The list comes behind the card (open() asked for it): its first game then, this one once the user is back on it.
    m_games = {game};
    m_game = 0;
    openCard();
    followRows();
    changed();
}

void DirectoryModel::reopen() {
    if (m_open) {
        return;
    }
    if (!m_opened) {
        open(DirectoryTab::Floors);
        return;
    }
    m_open = true;
    m_notice.clear();
    m_tabsFocused = false;
    m_tabsForEmpty = false;
    if (m_view == DirectoryView::Card) {
        if (m_closedCardGameId.empty()) {
            m_view = DirectoryView::Games;
            m_cardButton = DirectoryCardButton::Play;
        } else {
            // The card as it was: its content and standing are kept while the panel is closed.
            m_cardGameId = std::move(m_closedCardGameId);
        }
    }
    m_closedCardGameId.clear();
    followRows();
    changed();
}

void DirectoryModel::close() noexcept {
    if (!m_open) {
        return;
    }
    closePanel();
}

void DirectoryModel::closePanel() noexcept {
    m_open = false;
    m_tabsFocused = false;
    m_editFocused = false;
    m_closedCardGameId = std::move(m_cardGameId);
    m_cardGameId.clear();
    changed();
}

std::optional<DirectoryCommand> DirectoryModel::apply(UiCommand command) {
    if (!m_open) {
        return std::nullopt;
    }
    if (m_view == DirectoryView::Card) {
        switch (command) {
        case UiCommand::Left:
        case UiCommand::Up:
        case UiCommand::Right:
        case UiCommand::Down: {
            const bool forward = command == UiCommand::Right || command == UiCommand::Down;
            std::vector<DirectoryCardButton> shown;
            for (const DirectoryCardButton button : kDirectoryCardButtons) {
                if (button != DirectoryCardButton::ShowInRoom || cardShowsRoom()) {
                    shown.push_back(button);
                }
            }
            const auto at = std::find(shown.begin(), shown.end(), m_cardButton);
            const std::size_t index = at != shown.end() ? static_cast<std::size_t>(at - shown.begin()) : 0;
            const std::size_t next = forward ? std::min(index + 1, shown.size() - 1) : (index > 0 ? index - 1 : 0);
            if (shown[next] != m_cardButton) {
                m_cardButton = shown[next];
                changed();
            }
            return std::nullopt;
        }
        case UiCommand::Activate:
            return press(m_cardButton);
        case UiCommand::Back:
            return back();
        default:
            return std::nullopt;
        }
    }
    if (editFocused() && !m_tabsFocused) {
        // I4b step 3: "Modifica stanza" above the floors.
        switch (command) {
        case UiCommand::Up:
            m_editFocused = false;
            m_tabsFocused = true;
            m_tabsForEmpty = false;
            changed();
            return std::nullopt;
        case UiCommand::Down:
            m_editFocused = false;
            changed();
            return std::nullopt;
        case UiCommand::Activate:
            return leave(DirectoryCommandKind::EditRoom, m_building.editableRoom);
        case UiCommand::Back:
            return back();
        case UiCommand::FilterPrevious:
        case UiCommand::FilterNext:
            m_editFocused = false;
            switchTab(nextTab(m_tab, command == UiCommand::FilterNext, true));
            return std::nullopt;
        default:
            return std::nullopt;
        }
    }
    if (m_tabsFocused) {
        switch (command) {
        case UiCommand::Left:
        case UiCommand::Right:
            switchTab(nextTab(m_tab, command == UiCommand::Right, false));
            return std::nullopt;
        case UiCommand::FilterPrevious:
        case UiCommand::FilterNext:
            switchTab(nextTab(m_tab, command == UiCommand::FilterNext, true));
            return std::nullopt;
        case UiCommand::Down:
        case UiCommand::Activate:
            if (rowCount() > 0) {
                m_tabsFocused = false;
                m_tabsForEmpty = false;
                // I4b step 3: "Modifica stanza" stands between the tab bar and the floors.
                m_editFocused = editShown();
                changed();
            }
            return std::nullopt;
        case UiCommand::Back:
            return back();
        default:
            return std::nullopt;
        }
    }
    switch (command) {
    case UiCommand::Up:
        if (editShown() && rowCount() > 0 && selectedRow() == 0) {
            m_editFocused = true;
            changed();
            return std::nullopt;
        }
        move(-1, true);
        return std::nullopt;
    case UiCommand::Down:
        move(1, false);
        return std::nullopt;
    case UiCommand::PageUp:
        move(-static_cast<std::ptrdiff_t>(directory::kPageRows), false);
        return std::nullopt;
    case UiCommand::PageDown:
        move(static_cast<std::ptrdiff_t>(directory::kPageRows), false);
        return std::nullopt;
    case UiCommand::Home:
        if (rowCount() > 0 && selectedRow() != 0) {
            selectRow(0);
        }
        return std::nullopt;
    case UiCommand::End:
        if (rowCount() > 0 && selectedRow() != rowCount() - 1) {
            selectRow(rowCount() - 1);
        }
        return std::nullopt;
    case UiCommand::Activate:
        return activateRow();
    case UiCommand::Right:
        if (m_view == DirectoryView::Floors) {
            openRooms();
        }
        return std::nullopt;
    case UiCommand::Left:
        if (m_view == DirectoryView::Rooms) {
            return back();
        }
        return std::nullopt;
    case UiCommand::Back:
        return back();
    case UiCommand::FilterPrevious:
    case UiCommand::FilterNext:
        switchTab(nextTab(m_tab, command == UiCommand::FilterNext, true));
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

std::optional<DirectoryCommand> DirectoryModel::click(const DirectoryClick& click) {
    if (!m_open) {
        return std::nullopt;
    }
    switch (click.kind) {
    case DirectoryClick::Kind::Tab:
        switchTab(click.tab);
        if (m_tabsFocused && rowCount() > 0) {
            m_tabsFocused = false;
            m_tabsForEmpty = false;
            changed();
        }
        return std::nullopt;
    case DirectoryClick::Kind::Row:
        if (m_view == DirectoryView::Card) {
            if (click.row < m_games.size() && m_games[click.row].gameId == m_cardGameId) {
                // The row of the game whose card shows: the card stays as it is.
                return std::nullopt;
            }
            // Another row of the games list beside the card: its own card.
            m_view = DirectoryView::Games;
            m_cardGameId.clear();
            changed();
        }
        if (click.row >= rowCount()) {
            return std::nullopt;
        }
        m_tabsFocused = false;
        m_tabsForEmpty = false;
        selectRow(click.row);
        return activateRow();
    case DirectoryClick::Kind::Edit:
        if (!editShown()) {
            return std::nullopt;
        }
        return leave(DirectoryCommandKind::EditRoom, m_building.editableRoom);
    case DirectoryClick::Kind::Card:
        if (m_view != DirectoryView::Card ||
            (click.button == DirectoryCardButton::ShowInRoom && !cardShowsRoom())) {
            return std::nullopt;
        }
        if (m_cardButton != click.button) {
            m_cardButton = click.button;
            changed();
        }
        return press(click.button);
    }
    return std::nullopt;
}

std::string DirectoryModel::focusedElement() const {
    if (m_view == DirectoryView::Card) {
        return std::string(elementIdOf(m_cardButton));
    }
    if (m_tabsFocused || rowCount() == 0) {
        switch (m_tab) {
        case DirectoryTab::Floors:
            return std::string(directory::kFloorsTabId);
        case DirectoryTab::Games:
            return std::string(directory::kGamesTabId);
        case DirectoryTab::Changes:
            return std::string(directory::kChangesTabId);
        }
    }
    if (editFocused()) {
        return std::string(directory::kEditRoomId);
    }
    switch (m_view) {
    case DirectoryView::Floors:
        return std::string(directory::kFloorRowPrefix) + std::to_string(m_floor);
    case DirectoryView::Rooms:
        return std::string(directory::kRoomRowPrefix) + std::to_string(m_roomRow);
    case DirectoryView::Games:
    case DirectoryView::Card:
        return std::string(directory::kGameRowPrefix) + std::to_string(m_game);
    case DirectoryView::Changes:
        return std::string(directory::kActionRowPrefix) + std::to_string(m_action);
    }
    return {};
}

const DirectoryFloor* DirectoryModel::openedFloor() const noexcept {
    if (m_view != DirectoryView::Rooms || m_openedFloor >= m_building.floors.size()) {
        return nullptr;
    }
    return &m_building.floors[m_openedFloor];
}

std::size_t DirectoryModel::rowCount() const noexcept {
    switch (m_view) {
    case DirectoryView::Floors:
        return m_building.floors.size();
    case DirectoryView::Rooms: {
        const DirectoryFloor* floor = openedFloor();
        return floor != nullptr ? floor->rooms.size() + 1 : 0;
    }
    case DirectoryView::Games:
        return m_gamesState == DirectoryGamesState::Failed ? 0 : m_games.size();
    case DirectoryView::Card:
        return 0;
    case DirectoryView::Changes:
        return actions().size();
    }
    return 0;
}

std::size_t DirectoryModel::selectedRow() const noexcept {
    switch (m_view) {
    case DirectoryView::Floors:
        return m_floor;
    case DirectoryView::Rooms:
        return m_roomRow;
    case DirectoryView::Games:
    case DirectoryView::Card:
        return m_game;
    case DirectoryView::Changes:
        return m_action;
    }
    return 0;
}

bool DirectoryModel::cardShowsRoom() const noexcept {
    return m_cardRoom == quick_catalog::kRoomPlaced;
}

void DirectoryModel::move(std::ptrdiff_t delta, bool toTabs) {
    const std::size_t count = rowCount();
    const std::size_t row = selectedRow();
    if (count == 0 || (delta < 0 && row == 0)) {
        if (toTabs && !m_tabsFocused) {
            m_tabsFocused = true;
            m_tabsForEmpty = false;
            changed();
        }
        return;
    }
    const std::ptrdiff_t last = static_cast<std::ptrdiff_t>(count) - 1;
    const auto next = static_cast<std::size_t>(std::clamp(static_cast<std::ptrdiff_t>(row) + delta,
                                                          std::ptrdiff_t{0}, last));
    if (next != row) {
        selectRow(next);
    }
}

void DirectoryModel::selectRow(std::size_t row) {
    switch (m_view) {
    case DirectoryView::Floors:
        m_floor = row;
        break;
    case DirectoryView::Rooms:
        m_roomRow = row;
        break;
    case DirectoryView::Games:
    case DirectoryView::Card:
        m_game = row;
        m_gamesFromTop = false;
        break;
    case DirectoryView::Changes:
        m_action = row;
        break;
    }
    changed();
}

void DirectoryModel::switchTab(DirectoryTab tab) {
    if (tab == m_tab) {
        return;
    }
    m_tab = tab;
    m_editFocused = false;
    if (tab == DirectoryTab::Floors) {
        // Leaving a card: the games list keeps its selection.
        m_cardGameId.clear();
        m_view = m_floorsView == DirectoryView::Rooms && m_openedFloor < m_building.floors.size()
                     ? DirectoryView::Rooms
                     : DirectoryView::Floors;
    } else if (tab == DirectoryTab::Changes) {
        // I4b step 6: leaving a card too; the games list keeps its selection.
        m_cardGameId.clear();
        m_view = DirectoryView::Changes;
    } else {
        m_view = DirectoryView::Games;
        ++m_gamesRequest;
    }
    followRows();
    changed();
}

void DirectoryModel::openRooms() {
    if (m_floor >= m_building.floors.size()) {
        return;
    }
    m_editFocused = false;
    m_openedFloor = m_floor;
    m_roomRow = startRoomRow(m_floor);
    m_view = DirectoryView::Rooms;
    m_floorsView = DirectoryView::Rooms;
    changed();
}

void DirectoryModel::openCard() {
    if (m_game >= m_games.size()) {
        return;
    }
    const DirectoryGame& game = m_games[m_game];
    m_cardGameId = game.gameId;
    m_cardContent = GameCardContent{};
    m_cardContent.gameId = game.gameId;
    m_cardContent.title = game.title;
    m_cardContent.platform = game.systemName.empty() ? game.system : game.systemName;
    m_cardRoom.clear();
    m_cardButton = DirectoryCardButton::Play;
    m_view = DirectoryView::Card;
    m_gamesFromTop = false;
    ++m_cardRequest;
    changed();
}

std::optional<DirectoryCommand> DirectoryModel::back() {
    switch (m_view) {
    case DirectoryView::Card:
        m_view = DirectoryView::Games;
        m_cardGameId.clear();
        m_cardButton = DirectoryCardButton::Play;
        m_tabsFocused = false;
        m_tabsForEmpty = false;
        followRows();
        changed();
        return std::nullopt;
    case DirectoryView::Rooms:
        // Back to the floors with the list focused, from the tab bar too.
        m_view = DirectoryView::Floors;
        m_floorsView = DirectoryView::Floors;
        m_floor = m_openedFloor;
        m_tabsFocused = false;
        m_tabsForEmpty = false;
        followRows();
        changed();
        return std::nullopt;
    case DirectoryView::Floors:
    case DirectoryView::Games:
    case DirectoryView::Changes:
        return leave(DirectoryCommandKind::Close, {});
    }
    return std::nullopt;
}

std::optional<DirectoryCommand> DirectoryModel::activateRow() {
    switch (m_view) {
    case DirectoryView::Floors:
        openRooms();
        return std::nullopt;
    case DirectoryView::Rooms: {
        const DirectoryFloor* floor = openedFloor();
        if (floor == nullptr) {
            return std::nullopt;
        }
        if (m_roomRow == 0) {
            return leave(DirectoryCommandKind::ChooseFloor, floor->id);
        }
        if (m_roomRow > floor->rooms.size()) {
            return std::nullopt;
        }
        return leave(DirectoryCommandKind::ChooseRoom, floor->rooms[m_roomRow - 1].id);
    }
    case DirectoryView::Games:
        if (rowCount() > 0) {
            openCard();
        }
        return std::nullopt;
    case DirectoryView::Card:
        return std::nullopt;
    case DirectoryView::Changes: {
        // I4b step 6: an action asked of the app (a row that does nothing now: nothing).
        const std::vector<DirectoryAction> shown = actions();
        if (m_action >= shown.size() || !actionEnabled(shown[m_action])) {
            return std::nullopt;
        }
        switch (shown[m_action]) {
        case DirectoryAction::EditRoom:
            return leave(DirectoryCommandKind::EditRoom, m_building.editableRoom);
        case DirectoryAction::RegenerateRoom:
            return leave(DirectoryCommandKind::RegenerateRoom, m_building.regenerableRoom);
        case DirectoryAction::RegenerateFloor:
            return leave(DirectoryCommandKind::RegenerateFloor, m_building.currentFloor);
        case DirectoryAction::RegenerateWorld:
            return leave(DirectoryCommandKind::RegenerateWorld, {});
        case DirectoryAction::Undo:
            return leave(DirectoryCommandKind::Undo, {});
        }
        return std::nullopt;
    }
    }
    return std::nullopt;
}

std::optional<DirectoryCommand> DirectoryModel::press(DirectoryCardButton button) {
    switch (button) {
    case DirectoryCardButton::Play:
        return DirectoryCommand{DirectoryCommandKind::Launch, m_cardGameId};
    case DirectoryCardButton::ShowInRoom:
        if (!cardShowsRoom()) {
            return std::nullopt;
        }
        return leave(DirectoryCommandKind::ShowInRoom, m_cardGameId);
    case DirectoryCardButton::Back:
        return back();
    }
    return std::nullopt;
}

std::optional<DirectoryCommand> DirectoryModel::leave(DirectoryCommandKind kind, std::string id) {
    closePanel();
    return DirectoryCommand{kind, std::move(id)};
}

void DirectoryModel::followRows() noexcept {
    if (!m_open || m_view == DirectoryView::Card) {
        return;
    }
    if (rowCount() == 0) {
        if (!m_tabsFocused) {
            m_tabsFocused = true;
            m_tabsForEmpty = true;
        }
    } else if (m_tabsForEmpty) {
        m_tabsFocused = false;
        m_tabsForEmpty = false;
    }
}

std::size_t DirectoryModel::floorIndexOf(std::string_view id) const noexcept {
    if (id.empty()) {
        return m_building.floors.size();
    }
    const auto it = std::find_if(m_building.floors.begin(), m_building.floors.end(),
                                 [&](const DirectoryFloor& floor) { return floor.id == id; });
    return static_cast<std::size_t>(it - m_building.floors.begin());
}

std::size_t DirectoryModel::startRoomRow(std::size_t floor) const noexcept {
    const DirectoryFloor& shown = m_building.floors[floor];
    if (shown.id == m_building.currentFloor && !m_building.currentRoom.empty()) {
        for (std::size_t index = 0; index < shown.rooms.size(); ++index) {
            if (shown.rooms[index].id == m_building.currentRoom) {
                return index + 1;
            }
        }
    }
    return shown.rooms.empty() ? 0 : 1;
}

} // namespace rv::ui
