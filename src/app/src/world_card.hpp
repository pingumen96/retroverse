#pragma once

// The world's card beside the inspected item (I3 step 14a; ADR-0012 §5): while the interaction is in Inspect, the
// product UI's card (ui::GameCardScreen, through ProductUi::setWorldCard) shows the item's game: the catalog's row of
// it (AppCatalog::gameEntry, read again when the catalog's rows changed: the game's own readiness), the room's title
// until then, the plan's other versions of its title, the cover from the item's hero texture (registered for the
// document as rv-texture:game-card-cover through an RGBA8 view of its sRGB storage: the UI composites sRGB values) and
// the outcome of a launch that did not end normally. Its buttons have the focus: the arrows and the D-pad move it (the
// model's 2 x 2 grid), ui_activate (Enter, the pad's south button) presses the focused one, a click the one clicked;
// press() turns them into the interaction's InteractionFrame::card. The card shows in Inspect only, and hides under the
// quick catalog and the game-session panel (ProductUi's rule). Main thread only; destroy it while the OpenGL context is
// current.

#include <cstdint>
#include <optional>

#include <rv/app/run_report.hpp>
#include <rv/app/world_interaction.hpp>
#include <rv/input/action_state.hpp>
#include <rv/ui/catalog_source.hpp>
#include <rv/ui/game_card_model.hpp>
#include <rv/world/collection_layout.hpp>

namespace rv::gl {
class Texture2D;
} // namespace rv::gl

namespace rv::app {

class ProductUi;

class WorldCard {
public:
    WorldCard() = default;
    ~WorldCard();
    WorldCard(const WorldCard&) = delete;
    WorldCard& operator=(const WorldCard&) = delete;

    /// Before the interaction's update of frame `frame`: what the card's input presses (`actions`, the frame's actions
    /// in the World context or null while the product UI has the input; the clicks the product UI collected). The
    /// focus moves at once; a press waits for the item in the hand (the lift's press skips the lift alone).
    [[nodiscard]] std::optional<CardAction> press(const WorldInteraction& interaction,
                                                  const input::ActionState* actions, ProductUi& product,
                                                  std::uint64_t frame);
    /// After it: the card's content (Inspect of `item`) or none, given to the product UI with its cover; `hero` the
    /// item's resident hero texture (null: none yet), `otherVersions` the plan's other versions of its title. What the
    /// interaction did is recorded: its result's Vedi media and Informazioni, a launch (Inspect left for Launching).
    void update(const WorldInteraction& interaction, const InteractionResult& result, const world::CollectionItem* item,
                const gl::Texture2D* hero, std::uint32_t otherVersions, ProductUi& product, std::uint64_t frame);
    [[nodiscard]] const ui::GameCardModel& model() const noexcept { return m_model; }
    /// The run report's record (bound once a room's collection showed).
    void bind() noexcept { m_record.bound = true; }
    [[nodiscard]] const CardRecord& record() const noexcept { return m_record; }

private:
    void addEvent(std::uint64_t frame, std::string event, ui::CardButton button, std::string via = {},
                  std::string game = {});
    void addEvent(std::uint64_t frame, std::string event, std::string game = {});
    /// The cover's view of `hero` registered for the document (none: the key emptied).
    void setCover(const gl::Texture2D* hero, ProductUi& product);

    ui::GameCardModel m_model;
    /// The game shown, its catalog row (read once per game, again when the catalog's rows changed: the product UI's
    /// catalogRevision()).
    catalog::GameId m_game;
    std::optional<ui::CatalogEntry> m_entry;
    std::optional<std::uint64_t> m_entryRevision;
    /// The hero texture the cover's view was made of (its OpenGL name), and the view.
    unsigned m_coverSource = 0;
    unsigned m_coverView = 0;
    ui::CardButton m_lastFocus = ui::CardButton::Play;
    bool m_shown = false;
    /// The interaction's state the last update saw (Avvia leaves Inspect for Launching).
    WorldState m_lastState = WorldState::FreeRoam;
    CardRecord m_record;
};

} // namespace rv::app
