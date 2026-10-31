#include "world_card.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include <glad/gl.h>
#include <spdlog/spdlog.h>

#include <rv/app/world_card_ui.hpp>
#include <rv/render/texture.hpp>
#include <rv/scene/instance_data.hpp>

#include "product_ui.hpp"

namespace rv::app {
namespace {

/// The item is in the hand: the lift has ended (a press during it only skips it, ADR-0012 §4).
[[nodiscard]] bool inHand(const WorldInteraction& interaction) noexcept {
    return interaction.state() == WorldState::Inspect && interaction.inspectView().lift >= 1.0f;
}

} // namespace

WorldCard::~WorldCard() {
    if (m_coverView != 0) {
        glDeleteTextures(1, &m_coverView);
    }
}

std::optional<CardAction> WorldCard::press(const WorldInteraction& interaction, const input::ActionState* actions,
                                           ProductUi& product, std::uint64_t frame) {
    const std::optional<ui::CardButton> clicked = product.takeWorldCardClick();
    if (interaction.state() != WorldState::Inspect || actions == nullptr || !m_model.shown()) {
        return std::nullopt;
    }
    std::optional<ui::CardButton> pressed;
    std::string via;
    if (clicked) {
        if (inHand(interaction)) {
            pressed = m_model.click(*clicked);
            via = "click";
        }
    } else if (const auto command = cardCommandOf(actionsFor(WorldContext::Inspect, *actions))) {
        if (*command != ui::UiCommand::Activate) {
            static_cast<void>(m_model.apply(*command));
        } else if (inHand(interaction)) {
            pressed = m_model.apply(*command);
            via = "activate";
        }
    }
    if (m_model.focused() != m_lastFocus) {
        m_lastFocus = m_model.focused();
        addEvent(frame, "focus", m_lastFocus);
    }
    if (!pressed) {
        return std::nullopt;
    }
    addEvent(frame, "press", *pressed, via);
    spdlog::info("Frame {}: the card's {} pressed ({})", frame, ui::toString(*pressed), via);
    return cardActionOf(*pressed);
}

void WorldCard::update(const WorldInteraction& interaction, const InteractionResult& result,
                       const world::CollectionItem* item, const gl::Texture2D* hero,
                       const assets::LabelAspect& coverShape, std::uint32_t otherVersions, bool movable,
                       ProductUi& product, std::uint64_t frame) {
    const std::string game = item != nullptr ? item->game.toString() : std::string();
    // Avvia: Inspect left for the launch (13a's stand-in ends it at once, Returning, I3 step 15a launches).
    const WorldState state = interaction.state();
    if (m_lastState == WorldState::Inspect && (state == WorldState::AnimatingToConsole ||
                                               state == WorldState::Launching || state == WorldState::Returning)) {
        addEvent(frame, "launch", game);
    }
    m_lastState = state;
    if (result.showMedia) {
        addEvent(frame, "media", game);
    }
    if (result.showInfo) {
        addEvent(frame, "info", game);
    }
    if (interaction.state() != WorldState::Inspect || item == nullptr) {
        if (m_shown) {
            m_shown = false;
            m_model.hide();
            m_lastFocus = m_model.focused();
            product.setWorldCard(std::nullopt, ui::CardButton::Play);
            setCover(nullptr, product);
            m_record.focused.clear();
            addEvent(frame, "hidden");
        }
        return;
    }
    if (item->game != m_game) {
        m_game = item->game;
        m_entry.reset();
        m_entryRevision.reset();
    }
    // The catalog's row: once per game, again when the catalog's rows changed (the game's own readiness resolved).
    if (m_entryRevision != product.catalogRevision()) {
        m_entry = product.catalog().gameEntry(m_game);
        m_entryRevision = product.catalogRevision();
    }
    setCover(hero, product);
    ui::GameCardContent content =
        cardContentOf(item->game, item->title, (item->flags & scene::instance_flags::kUnavailable) != 0, m_entry,
                      otherVersions, m_coverView != 0, interaction.notice(), interaction.noticeMessage());
    // The hero picture's shape: the card shows it at that aspect (the layer's own, 0 x 0, fills the frame as in I3).
    if (!coverShape.isLayer()) {
        content.coverWidth = coverShape.width;
        content.coverHeight = coverShape.height;
    }
    content.movable = movable;
    const bool newGame = !m_shown || m_model.gameId() != content.gameId;
    m_model.show(content.gameId, movable);
    product.setWorldCard(content, m_model.focused());
    if (newGame) {
        addEvent(frame, "shown", content.gameId);
    }
    m_shown = true;
    if (m_model.focused() != m_lastFocus) {
        m_lastFocus = m_model.focused();
        addEvent(frame, "focus", m_lastFocus);
    }

    ++m_record.shownFrames;
    if (content.cover) {
        ++m_record.coverFrames;
    }
    if (product.worldCardSqueezed()) {
        ++m_record.squeezedFrames;
    }
    m_record.game = content.gameId;
    m_record.title = content.title;
    m_record.platform = content.platform;
    m_record.year = content.year > 0 ? static_cast<std::uint64_t>(content.year) : 0;
    m_record.readiness = content.readiness;
    m_record.blocker = content.blocker;
    m_record.details = content.details;
    m_record.otherVersions = content.otherVersions;
    m_record.focused = std::string(ui::toString(m_model.focused()));
    if (const std::optional<glm::vec4> box = product.worldCardCoverBox()) {
        m_record.coverBox = {box->x, box->y, box->z, box->w};
    }
}

void WorldCard::setCover(const gl::Texture2D* hero, ProductUi& product) {
    const unsigned source = hero != nullptr && hero->valid() ? hero->id() : 0;
    if (source == m_coverSource) {
        return;
    }
    if (m_coverView != 0) {
        product.setUiTexture(ui::game_card::kCoverTexture, 0, {});
        glDeleteTextures(1, &m_coverView);
        m_coverView = 0;
    }
    m_coverSource = source;
    if (source == 0) {
        return;
    }
    // The UI composites sRGB-encoded values (RmlUi blends in sRGB, GL_FRAMEBUFFER_SRGB off): the hero texture's sRGB
    // storage is read through an RGBA8 view of it, its texels as they are, with its mips.
    GLuint view = 0;
    glGenTextures(1, &view);
    glTextureView(view, GL_TEXTURE_2D, source, GL_RGBA8, 0, static_cast<GLuint>(std::max(1, hero->mipLevels())), 0, 1);
    glTextureParameteri(view, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(view, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(view, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(view, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const std::string label = "WorldCard.Cover";
    glObjectLabel(GL_TEXTURE, view, static_cast<GLsizei>(label.size()), label.c_str());
    m_coverView = view;
    product.setUiTexture(ui::game_card::kCoverTexture, m_coverView, hero->size());
}

void WorldCard::addEvent(std::uint64_t frame, std::string event, ui::CardButton button, std::string via,
                         std::string game) {
    if (m_record.events.size() >= kMaxInteractionEvents) {
        ++m_record.eventsDropped;
        return;
    }
    m_record.events.push_back(
        CardEventRecord{frame, std::move(event), std::string(ui::toString(button)), std::move(via), std::move(game)});
}

void WorldCard::addEvent(std::uint64_t frame, std::string event, std::string game) {
    if (m_record.events.size() >= kMaxInteractionEvents) {
        ++m_record.eventsDropped;
        return;
    }
    m_record.events.push_back(CardEventRecord{frame, std::move(event), {}, {}, std::move(game)});
}

} // namespace rv::app
