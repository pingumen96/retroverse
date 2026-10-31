#pragma once

// The app's world loop around app::WorldInteraction, without a window (I3 step 7): hand-built shelves
// (tests/unit/world/synthetic_shelves.hpp) as the room, the walker (scene::FirstPersonController) and its camera, the
// reticle's ray sampled by the room's world::PickIndex each frame, the results applied as I3 steps 13a and 13b will
// apply them (the walker placed where an ease drives it, else given its context's actions).

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <rv/app/world_interaction.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/input/action.hpp>
#include <rv/input/action_state.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/first_person_controller.hpp>
#include <rv/world/pick_index.hpp>
#include <rv/world/slot_navigation.hpp>

#include "../world/synthetic_shelves.hpp"

namespace rv::test {

inline constexpr float kFrameSeconds = 0.016667f;
inline constexpr float kEyeHeight = 1.65f;

/// The walker standing `distance` in front of `item`'s face, looking at its middle.
[[nodiscard]] inline scene::WalkerPose facing(const SyntheticItem& item, float distance) {
    scene::WalkerPose pose;
    const glm::vec3 eye = item.faceCenter + distance * item.out;
    pose.feet = glm::vec3(eye.x, 0.0f, eye.z);
    const glm::vec3 forward = -item.out;
    pose.yaw = std::atan2(forward.x, -forward.z);
    pose.pitch = std::atan2(item.faceCenter.y - kEyeHeight, distance);
    return pose;
}

/// The shelves as app::InteractionRoom: every item a game (GameId 1000 + its pick id), its section its module, its
/// Focus pose 0.9 m in front of it; items can be taken out of the room (the plan changed) while the pick index still
/// holds them.
class ShelfRoom final : public app::InteractionRoom {
public:
    explicit ShelfRoom(SyntheticShelves shelves) : m_shelves(std::move(shelves)) {
        m_index = world::PickIndex::build(m_shelves.targets, m_shelves.occluders).value();
        m_navigation = world::SlotNavigation::build({m_shelves.run}).value();
    }

    [[nodiscard]] std::optional<app::RoomItem> item(world::PickId id) const override {
        if (std::find(m_removed.begin(), m_removed.end(), id) != m_removed.end()) {
            return std::nullopt;
        }
        for (const SyntheticItem& candidate : m_shelves.items) {
            if (candidate.id == id) {
                return app::RoomItem{id, gameOf(id), candidate.faceOut, static_cast<std::uint32_t>(candidate.module)};
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] scene::WalkerPose focusPose(world::PickId id) const override {
        return facing(m_shelves.item(id), 0.9f);
    }

    [[nodiscard]] std::optional<world::PickId> step(world::PickId from,
                                                    world::StepDirection direction) const override {
        return m_navigation.step(from, direction);
    }

    /// I5 step 9: the walker at `from`'s feet looking at `console` from its eye; nothing without one.
    [[nodiscard]] std::optional<scene::WalkerPose> consolePose(const scene::WalkerPose& from) const override {
        if (!console) {
            return std::nullopt;
        }
        const glm::vec3 to = *console - (from.feet + glm::vec3(0.0f, kEyeHeight, 0.0f));
        return scene::WalkerPose{from.feet, std::atan2(to.x, -to.z),
                                 std::atan2(to.y, std::sqrt(to.x * to.x + to.z * to.z))};
    }

    void remove(world::PickId id) { m_removed.push_back(id); }

    /// I5 step 9: where the room's console's media socket stands (none: a room without a console, the default).
    std::optional<glm::vec3> console;

    [[nodiscard]] static catalog::GameId gameOf(world::PickId id) {
        return catalog::GameId::fromValue(1000 + static_cast<std::uint64_t>(id)).value();
    }

    [[nodiscard]] const SyntheticShelves& shelves() const noexcept { return m_shelves; }
    [[nodiscard]] const world::PickIndex& index() const noexcept { return m_index; }
    [[nodiscard]] world::PickId at(int module, int level, int slot) const {
        return m_shelves.at(module, level, slot)->id;
    }

private:
    SyntheticShelves m_shelves;
    world::PickIndex m_index;
    world::SlotNavigation m_navigation;
    std::vector<world::PickId> m_removed;
};

/// Default shelves: two side-wall modules and a third one whose level 4 is the face-out band.
[[nodiscard]] inline SyntheticShelves harnessShelves() {
    std::vector<ShelfModuleSpec> modules(3);
    modules[2].faceOutLevel = 4;
    return buildShelves(modules);
}

/// The actions of a frame in which `actions` went down (held and pressed).
[[nodiscard]] inline input::ActionState pressing(std::initializer_list<input::Action> actions) {
    input::ActionState state;
    for (const input::Action action : actions) {
        state.setButton(action, true, true, false);
    }
    return state;
}

/// One frame's record, for comparing runs.
struct FrameRecord {
    app::WorldState state = app::WorldState::FreeRoam;
    world::PickId hovered = world::kNoPick;
    world::PickId focused = world::kNoPick;
    scene::WalkerPose walker;
    std::vector<app::PulledItem> pulled;
    app::InspectView inspect;
    bool walkerDriven = false;

    bool operator==(const FrameRecord& other) const {
        return state == other.state && hovered == other.hovered && focused == other.focused &&
               walker.feet == other.walker.feet && walker.yaw == other.walker.yaw &&
               walker.pitch == other.walker.pitch && pulled == other.pulled && inspect == other.inspect &&
               walkerDriven == other.walkerDriven;
    }
};

class InteractionHarness {
public:
    explicit InteractionHarness(const scene::WalkerPose& spawn, app::InteractionSettings settings = {},
                                SyntheticShelves shelves = harnessShelves())
        : room(std::move(shelves)), interaction(settings) {
        interaction.setRoom(&room);
        place(spawn);
    }

    /// One frame of the app's loop: the reticle's ray sampled, the interaction updated, its walker pose applied or the
    /// walker given its context's actions. `actions` nullptr: the product UI has the input.
    app::InteractionResult frame(const input::ActionState* actions, float dt = kFrameSeconds, float wheelNotches = 0.0f,
                                 std::optional<app::CardAction> card = std::nullopt) {
        app::InteractionFrame frame;
        frame.actions = actions;
        frame.card = card;
        frame.deltaSeconds = dt;
        frame.walker = pose();
        frame.hover = room.index().sample(camera.rayFromNdc({0.0f, 0.0f}), interaction.hovered());
        frame.wheelNotches = wheelNotches;
        frame.back = back;
        const app::WorldContext context = interaction.context();
        last = interaction.update(frame);
        if (last.walker) {
            place(*last.walker);
        } else {
            const input::ActionState none;
            walker.update(app::walkerActionsFor(context, actions ? *actions : none), dt, camera);
        }
        ++frames;
        return last;
    }

    app::InteractionResult frame(const input::ActionState& actions, float dt = kFrameSeconds,
                                 float wheelNotches = 0.0f) {
        return frame(&actions, dt, wheelNotches);
    }

    /// One frame in which the card's button `card` is pressed (I3 step 14a), with `actions`.
    app::InteractionResult press(app::CardAction card, const input::ActionState& actions = {}) {
        return frame(&actions, kFrameSeconds, 0.0f, card);
    }

    /// `count` frames without input; the last result.
    app::InteractionResult idle(int count, float dt = kFrameSeconds) {
        const input::ActionState none;
        app::InteractionResult result;
        for (int i = 0; i < count; ++i) {
            result = frame(none, dt);
        }
        return result;
    }

    void place(const scene::WalkerPose& pose) {
        walker.teleport(pose.feet, pose.yaw, pose.pitch);
        walker.update(input::ActionState{}, 0.0f, camera);
    }

    [[nodiscard]] scene::WalkerPose pose() const {
        return scene::WalkerPose{walker.feetPosition(), walker.yaw(), walker.pitch()};
    }

    [[nodiscard]] FrameRecord record() const {
        return FrameRecord{interaction.state(),
                           interaction.hovered(),
                           interaction.focused(),
                           pose(),
                           std::vector<app::PulledItem>(interaction.pulledItems().begin(),
                                                        interaction.pulledItems().end()),
                           interaction.inspectView(),
                           last.walker.has_value()};
    }

    ShelfRoom room;
    app::WorldInteraction interaction;
    scene::FirstPersonController walker;
    scene::Camera camera;
    app::BackState back{.mouseCaptured = true};
    app::InteractionResult last;
    int frames = 0;
};

} // namespace rv::test
