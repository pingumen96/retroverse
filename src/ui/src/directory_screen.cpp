#include <rv/ui/directory_screen.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ScrollTypes.h>
#include <SDL3/SDL_events.h>
#include <fmt/format.h>

#include <rv/ui/ui_view.hpp>

namespace rv::ui {
namespace {

/// A floor of the floors list (directory.rml's "floors").
struct FloorRow {
    Rml::String elementId;
    int index = 0;
    Rml::String id;
    /// The document names the floor (directory::kFloorIds): else it shows the id.
    bool known = false;
    int order = 0;
    int rooms = 0;
    int games = 0;
    bool current = false;
};

/// The floor whose rooms are listed ("shown").
struct ShownFloor {
    Rml::String id;
    bool known = false;
    int order = 0;
    /// The walker is on it.
    bool current = false;
};

/// A room of the rooms list ("rooms"): row index + 1 (row 0 is the hall).
struct RoomRow {
    Rml::String elementId;
    int index = 0;
    Rml::String id;
    Rml::String kind;
    /// A console room's system name as shown (its full name, else its name).
    Rml::String title;
    /// Its sections' systems as shown.
    std::vector<Rml::String> systems;
    int games = 0;
    bool current = false;
};

/// A game of the favourites and recent games ("games").
struct GameRow {
    Rml::String elementId;
    int index = 0;
    Rml::String title;
    Rml::String systemName;
    bool favorite = false;
    /// "dd/mm/yyyy", or "" never played.
    Rml::String played;
    /// Its card shows.
    bool chosen = false;
};

/// I4b step 6: a row of the "Modifiche" tab ("actions").
struct ActionRow {
    Rml::String elementId;
    int index = 0;
    /// directory::kActions' name.
    Rml::String action;
    /// It does something now (else its line says why and it is dimmed).
    bool enabled = false;
};

/// The card ("card"): GameCardContent's fields as game_card_screen.cpp binds them, and where the game stands.
struct CardData {
    Rml::String title;
    Rml::String platform;
    Rml::String year;
    Rml::String developer;
    Rml::String genre;
    Rml::String players;
    Rml::String state;
    Rml::String readiness;
    Rml::String blocker;
    bool verified = false;
    bool details = false;
    int otherVersions = 0;
    Rml::String outcome;
    bool outcomeKnown = false;
    Rml::String room;
};

[[nodiscard]] bool knownFloor(std::string_view id) noexcept {
    return std::find(directory::kFloorIds.begin(), directory::kFloorIds.end(), id) != directory::kFloorIds.end();
}

[[nodiscard]] int count(std::size_t value) noexcept {
    return static_cast<int>(std::min<std::size_t>(value, 1'000'000'000));
}

[[nodiscard]] Rml::String shownName(const DirectorySystem& system) {
    return Rml::String(system.fullName.empty() ? system.name : system.fullName);
}

[[nodiscard]] bool isPointerButton(Uint32 type) noexcept {
    return type == SDL_EVENT_MOUSE_BUTTON_DOWN || type == SDL_EVENT_MOUSE_BUTTON_UP;
}

[[nodiscard]] bool isPointer(Uint32 type) noexcept {
    return isPointerButton(type) || type == SDL_EVENT_MOUSE_MOTION || type == SDL_EVENT_MOUSE_WHEEL ||
           type == SDL_EVENT_WINDOW_MOUSE_LEAVE;
}

} // namespace

struct DirectoryScreen::Impl {
    explicit Impl(UiView& view) : view(view) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (document != nullptr) {
            view.context().UnloadDocument(document);
        }
        if (modelCreated) {
            view.context().RemoveDataModel(Rml::String(directory::kDataModel));
        }
    }

    Status registerDataModel() {
        Rml::DataModelConstructor constructor = view.context().CreateDataModel(Rml::String(directory::kDataModel));
        if (!constructor) {
            return makeError(ErrorCode::InvalidArgument, "the UI view has this data model already",
                             std::string(directory::kDataModel));
        }
        modelCreated = true;
        if (auto floor = constructor.RegisterStruct<FloorRow>()) {
            floor.RegisterMember("elementId", &FloorRow::elementId);
            floor.RegisterMember("index", &FloorRow::index);
            floor.RegisterMember("id", &FloorRow::id);
            floor.RegisterMember("known", &FloorRow::known);
            floor.RegisterMember("order", &FloorRow::order);
            floor.RegisterMember("rooms", &FloorRow::rooms);
            floor.RegisterMember("games", &FloorRow::games);
            floor.RegisterMember("current", &FloorRow::current);
        }
        constructor.RegisterArray<std::vector<FloorRow>>();
        if (auto shownFloor = constructor.RegisterStruct<ShownFloor>()) {
            shownFloor.RegisterMember("id", &ShownFloor::id);
            shownFloor.RegisterMember("known", &ShownFloor::known);
            shownFloor.RegisterMember("order", &ShownFloor::order);
            shownFloor.RegisterMember("current", &ShownFloor::current);
        }
        constructor.RegisterArray<std::vector<Rml::String>>();
        if (auto room = constructor.RegisterStruct<RoomRow>()) {
            room.RegisterMember("elementId", &RoomRow::elementId);
            room.RegisterMember("index", &RoomRow::index);
            room.RegisterMember("id", &RoomRow::id);
            room.RegisterMember("kind", &RoomRow::kind);
            room.RegisterMember("title", &RoomRow::title);
            room.RegisterMember("systems", &RoomRow::systems);
            room.RegisterMember("games", &RoomRow::games);
            room.RegisterMember("current", &RoomRow::current);
        }
        constructor.RegisterArray<std::vector<RoomRow>>();
        if (auto game = constructor.RegisterStruct<GameRow>()) {
            game.RegisterMember("elementId", &GameRow::elementId);
            game.RegisterMember("index", &GameRow::index);
            game.RegisterMember("title", &GameRow::title);
            game.RegisterMember("systemName", &GameRow::systemName);
            game.RegisterMember("favorite", &GameRow::favorite);
            game.RegisterMember("played", &GameRow::played);
            game.RegisterMember("chosen", &GameRow::chosen);
        }
        constructor.RegisterArray<std::vector<GameRow>>();
        if (auto action = constructor.RegisterStruct<ActionRow>()) {
            action.RegisterMember("elementId", &ActionRow::elementId);
            action.RegisterMember("index", &ActionRow::index);
            action.RegisterMember("action", &ActionRow::action);
            action.RegisterMember("enabled", &ActionRow::enabled);
        }
        constructor.RegisterArray<std::vector<ActionRow>>();
        if (auto card = constructor.RegisterStruct<CardData>()) {
            card.RegisterMember("title", &CardData::title);
            card.RegisterMember("platform", &CardData::platform);
            card.RegisterMember("year", &CardData::year);
            card.RegisterMember("developer", &CardData::developer);
            card.RegisterMember("genre", &CardData::genre);
            card.RegisterMember("players", &CardData::players);
            card.RegisterMember("state", &CardData::state);
            card.RegisterMember("readiness", &CardData::readiness);
            card.RegisterMember("blocker", &CardData::blocker);
            card.RegisterMember("verified", &CardData::verified);
            card.RegisterMember("details", &CardData::details);
            card.RegisterMember("otherVersions", &CardData::otherVersions);
            card.RegisterMember("outcome", &CardData::outcome);
            card.RegisterMember("outcomeKnown", &CardData::outcomeKnown);
            card.RegisterMember("room", &CardData::room);
        }
        constructor.Bind("view", &viewName);
        constructor.Bind("tab", &tabName);
        constructor.Bind("gamesState", &gamesState);
        constructor.Bind("notice", &notice);
        constructor.Bind("floorCount", &floorCount);
        constructor.Bind("gameCount", &gameCount);
        constructor.Bind("hallCurrent", &hallCurrent);
        constructor.Bind("editShown", &editShown);
        constructor.Bind("editFocused", &editFocused);
        constructor.Bind("editKind", &editKind);
        constructor.Bind("editTitle", &editTitle);
        // I4b step 6: the "Modifiche" tab: its rows, the room "Rigenera stanza" names, the walker's floor.
        constructor.Bind("actions", &actions);
        constructor.Bind("actionRoomKind", &actionRoomKind);
        constructor.Bind("actionRoomTitle", &actionRoomTitle);
        constructor.Bind("actionFloor", &actionFloor);
        constructor.Bind("actionFloorKnown", &actionFloorKnown);
        constructor.BindEventCallback(Rml::String(directory::kEditEvent),
                                      [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
                                          DirectoryClick click;
                                          click.kind = DirectoryClick::Kind::Edit;
                                          clicked = click;
                                      });
        constructor.Bind("floors", &floors);
        constructor.Bind("shown", &shown);
        constructor.Bind("rooms", &rooms);
        constructor.Bind("games", &games);
        constructor.Bind("card", &card);
        constructor.BindEventCallback(Rml::String(directory::kTabEvent),
                                      [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                                          if (args.empty()) {
                                              return;
                                          }
                                          if (const auto tab = directoryTabFromName(args.front().Get<Rml::String>())) {
                                              DirectoryClick click;
                                              click.kind = DirectoryClick::Kind::Tab;
                                              click.tab = *tab;
                                              clicked = click;
                                          }
                                      });
        constructor.BindEventCallback(Rml::String(directory::kRowEvent),
                                      [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                                          if (args.empty()) {
                                              return;
                                          }
                                          const int row = args.front().Get<int>(-1);
                                          if (row >= 0) {
                                              DirectoryClick click;
                                              click.kind = DirectoryClick::Kind::Row;
                                              click.row = static_cast<std::size_t>(row);
                                              clicked = click;
                                          }
                                      });
        constructor.BindEventCallback(
            Rml::String(directory::kCardEvent),
            [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                if (args.empty()) {
                    return;
                }
                if (const auto button = directoryCardButtonFromName(args.front().Get<Rml::String>())) {
                    DirectoryClick click;
                    click.kind = DirectoryClick::Kind::Card;
                    click.button = *button;
                    clicked = click;
                }
            });
        handle = constructor.GetModelHandle();
        return ok();
    }

    Status loadDocument() {
        document = view.context().LoadDocument(Rml::String(directory::kDocumentPath));
        if (document == nullptr) {
            return makeError(ErrorCode::IoError, "the directory's document could not be loaded (see the log)",
                             std::string(directory::kDocumentPath));
        }
        document->Hide();
        return ok();
    }

    /// Everything the document binds, from `model`.
    void bind(const DirectoryModel& model) {
        viewName = Rml::String(toString(model.view()));
        tabName = Rml::String(toString(model.tab()));
        gamesState = Rml::String(toString(model.gamesState()));
        notice = Rml::String(model.notice());

        const DirectoryBuilding& building = model.building();
        floorCount = count(building.floors.size());
        floors.clear();
        for (std::size_t index = 0; index < building.floors.size(); ++index) {
            const DirectoryFloor& floor = building.floors[index];
            FloorRow row;
            row.elementId = Rml::String(directory::kFloorRowPrefix) + std::to_string(index);
            row.index = count(index);
            row.id = Rml::String(floor.id);
            row.known = knownFloor(floor.id);
            row.order = count(floor.order);
            row.rooms = count(floor.rooms.size());
            std::size_t placed = 0;
            for (const DirectoryRoom& room : floor.rooms) {
                placed += room.games;
            }
            row.games = count(placed);
            row.current = floor.id == building.currentFloor;
            floors.push_back(std::move(row));
        }

        // I4b step 3: the room "Modifica stanza" edits, named as its row is.
        editShown = model.editShown();
        editFocused = model.editFocused();
        editKind.clear();
        editTitle.clear();
        if (editShown) {
            for (const DirectoryFloor& floor : building.floors) {
                for (const DirectoryRoom& room : floor.rooms) {
                    if (room.id == building.editableRoom) {
                        editKind = Rml::String(room.kind);
                        editTitle = room.systems.empty() ? Rml::String(room.id) : shownName(room.systems.front());
                    }
                }
            }
        }

        // I4b step 6: the "Modifiche" tab.
        actions.clear();
        const std::vector<DirectoryAction> shownActions = model.actions();
        for (std::size_t index = 0; index < shownActions.size(); ++index) {
            ActionRow row;
            row.elementId = Rml::String(directory::kActionRowPrefix) + std::to_string(index);
            row.index = count(index);
            row.action = Rml::String(toString(shownActions[index]));
            row.enabled = model.actionEnabled(shownActions[index]);
            actions.push_back(std::move(row));
        }
        actionRoomKind.clear();
        actionRoomTitle.clear();
        for (const DirectoryFloor& floor : building.floors) {
            for (const DirectoryRoom& room : floor.rooms) {
                if (!building.regenerableRoom.empty() && room.id == building.regenerableRoom) {
                    actionRoomKind = Rml::String(room.kind);
                    actionRoomTitle = room.systems.empty() ? Rml::String(room.id) : shownName(room.systems.front());
                }
            }
        }
        if (actionRoomKind.empty() && !building.regenerableRoom.empty()) {
            // A room the floors do not list (an expansion, step 8's): named by its id.
            actionRoomKind = "console";
            actionRoomTitle = Rml::String(building.regenerableRoom);
        }
        actionFloor = Rml::String(building.currentFloor);
        actionFloorKnown = knownFloor(building.currentFloor);

        shown = ShownFloor{};
        rooms.clear();
        hallCurrent = false;
        if (const DirectoryFloor* floor = model.openedFloor()) {
            shown.id = Rml::String(floor->id);
            shown.known = knownFloor(floor->id);
            shown.order = count(floor->order);
            shown.current = floor->id == building.currentFloor;
            hallCurrent = shown.current && building.currentRoom.empty();
            for (std::size_t index = 0; index < floor->rooms.size(); ++index) {
                const DirectoryRoom& room = floor->rooms[index];
                RoomRow row;
                row.elementId = Rml::String(directory::kRoomRowPrefix) + std::to_string(index + 1);
                row.index = count(index + 1);
                row.id = Rml::String(room.id);
                row.kind = Rml::String(room.kind);
                row.title = room.systems.empty() ? Rml::String(room.id) : shownName(room.systems.front());
                for (const DirectorySystem& system : room.systems) {
                    row.systems.push_back(shownName(system));
                }
                row.games = count(room.games);
                row.current = shown.current && room.id == building.currentRoom;
                rooms.push_back(std::move(row));
            }
        }

        const bool cardShown = model.view() == DirectoryView::Card;
        gameCount = count(model.games().size());
        games.clear();
        for (std::size_t index = 0; index < model.games().size(); ++index) {
            const DirectoryGame& game = model.games()[index];
            GameRow row;
            row.elementId = Rml::String(directory::kGameRowPrefix) + std::to_string(index);
            row.index = count(index);
            row.title = Rml::String(game.title);
            row.systemName = Rml::String(game.systemName.empty() ? game.system : game.systemName);
            row.favorite = game.favorite;
            if (game.played) {
                row.played = Rml::String(
                    fmt::format("{:02}/{:02}/{:04}", game.played->day, game.played->month, game.played->year));
            }
            row.chosen = cardShown && index == model.selectedGame();
            games.push_back(std::move(row));
        }

        card = CardData{};
        if (cardShown) {
            const GameCardContent& content = model.cardContent();
            card.title = Rml::String(content.title);
            card.platform = Rml::String(content.platform);
            card.year = content.year > 0 ? Rml::String(std::to_string(content.year)) : Rml::String();
            card.developer = Rml::String(content.developer);
            card.genre = Rml::String(content.genre);
            card.players = Rml::String(content.players);
            card.state = Rml::String(content.state);
            card.readiness = Rml::String(content.readiness);
            card.blocker = Rml::String(content.blocker);
            card.verified = content.verified;
            card.details = content.details;
            card.otherVersions = static_cast<int>(std::min<std::uint32_t>(content.otherVersions, 1'000'000));
            card.outcome = Rml::String(content.outcome);
            card.outcomeKnown = std::find(game_card::kOutcomeCodes.begin(), game_card::kOutcomeCodes.end(),
                                          content.outcome) != game_card::kOutcomeCodes.end();
            card.room = Rml::String(model.cardRoom());
        }
        handle.DirtyAllVariables();
        ++bindCount;
    }

    /// The model's element focused and scrolled into view, unless it has the focus already.
    void applyFocus() {
        if (model == nullptr) {
            return;
        }
        const std::string id = model->focusedElement();
        Rml::Element* element = document->GetElementById(Rml::String(id));
        if (element == nullptr || view.context().GetFocusElement() == element) {
            return;
        }
        if (element->Focus(true)) {
            element->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
        }
    }

    UiView& view;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle handle;
    bool modelCreated = false;
    bool documentShown = false;
    const DirectoryModel* model = nullptr;
    std::optional<std::uint64_t> boundRevision;
    std::uint64_t bindCount = 0;
    std::optional<DirectoryClick> clicked;

    Rml::String viewName;
    Rml::String tabName;
    Rml::String gamesState;
    Rml::String notice;
    int floorCount = 0;
    int gameCount = 0;
    bool hallCurrent = false;
    /// I4b step 3: "Modifica stanza": shown, the room's kind and title.
    bool editShown = false;
    bool editFocused = false;
    Rml::String editKind;
    Rml::String editTitle;
    /// I4b step 6: the "Modifiche" tab.
    std::vector<ActionRow> actions;
    Rml::String actionRoomKind;
    Rml::String actionRoomTitle;
    Rml::String actionFloor;
    bool actionFloorKnown = false;
    std::vector<FloorRow> floors;
    ShownFloor shown;
    std::vector<RoomRow> rooms;
    std::vector<GameRow> games;
    CardData card;
};

DirectoryScreen::DirectoryScreen(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

DirectoryScreen::~DirectoryScreen() = default;

Expected<std::unique_ptr<DirectoryScreen>> DirectoryScreen::create(UiView& view) {
    auto impl = std::make_unique<Impl>(view);
    if (auto registered = impl->registerDataModel(); !registered) {
        return propagate(registered.error());
    }
    if (auto loaded = impl->loadDocument(); !loaded) {
        return propagate(loaded.error());
    }
    return std::unique_ptr<DirectoryScreen>(new DirectoryScreen(std::move(impl)));
}

void DirectoryScreen::show(const DirectoryModel& model) {
    Impl& impl = *m_impl;
    if (impl.document == nullptr) {
        return;
    }
    impl.model = &model;
    if (impl.boundRevision != model.revision()) {
        impl.bind(model);
        impl.boundRevision = model.revision();
    }
    if (!impl.documentShown) {
        // Over the documents loaded before it, without taking the input from the application: the focus goes to the
        // model's element after the next update.
        impl.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        impl.document->PullToFront();
        impl.documentShown = true;
    }
}

void DirectoryScreen::hide() {
    Impl& impl = *m_impl;
    impl.clicked.reset();
    impl.model = nullptr;
    if (!impl.documentShown || impl.document == nullptr) {
        impl.documentShown = false;
        return;
    }
    impl.documentShown = false;
    impl.document->Hide();
    // Hide() gives the focus back to the document focused before, when there is one; else it stays on an element of
    // this hidden document (Blur() would hand it to the parent, and the context's root refuses it): the document
    // itself takes it, RmlUi's "no element focused" (UiView::focusedElement()).
    if (const Rml::Element* focus = impl.view.context().GetFocusElement();
        focus != nullptr && focus->GetOwnerDocument() == impl.document && focus != impl.document) {
        static_cast<void>(impl.document->Focus());
    }
}

bool DirectoryScreen::visible() const noexcept {
    return m_impl->documentShown;
}

void DirectoryScreen::afterUpdate() {
    Impl& impl = *m_impl;
    if (!impl.documentShown || impl.document == nullptr) {
        return;
    }
    impl.applyFocus();
}

bool DirectoryScreen::handlePointer(const SDL_Event& event) {
    Impl& impl = *m_impl;
    if (!impl.documentShown || impl.document == nullptr || !isPointer(event.type)) {
        return false;
    }
    static_cast<void>(impl.view.handleEvent(event));
    if (!isPointerButton(event.type)) {
        return false;
    }
    // Over one of its panels (the body covers the view: a click beside them belongs to the world).
    const Rml::Element* hover = impl.view.context().GetHoverElement();
    return hover != nullptr && hover->GetOwnerDocument() == impl.document && hover != impl.document;
}

std::optional<DirectoryClick> DirectoryScreen::takeClicked() noexcept {
    return std::exchange(m_impl->clicked, std::nullopt);
}

std::string DirectoryScreen::focusedElement() const {
    const Rml::Element* focus = m_impl->view.context().GetFocusElement();
    if (focus == nullptr || focus->GetOwnerDocument() != m_impl->document) {
        return {};
    }
    return std::string(focus->GetId());
}

bool DirectoryScreen::cardSqueezed() const {
    const Impl& impl = *m_impl;
    if (!impl.documentShown || impl.document == nullptr) {
        return false;
    }
    // RmlUi's measures are not const (they may lay the element out).
    Rml::Element* card = impl.document->GetElementById(Rml::String(directory::kCardId));
    return card != nullptr && card->GetScrollHeight() > card->GetClientHeight() + 1.0f;
}

std::uint64_t DirectoryScreen::binds() const noexcept {
    return m_impl->bindCount;
}

} // namespace rv::ui
