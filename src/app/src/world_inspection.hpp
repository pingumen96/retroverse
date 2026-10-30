#pragma once

// The inspection in the app (I3 step 13b; ADR-0012 §4): the inspected item drawn on the dynamic path from its
// pulled-out pose into the hand (heldItemTransform(): upright in front of the eye, its cover facing it, turned and
// zoomed as WorldInteraction's InspectView says, kept out of its own fixture), the scene behind it dimmed by one stop
// and vignetted while it is lifted (gl::OutlineSettings::dimOutside over the outline's mark of the item, the view's
// PostSettings::vignette), and the run report's world.interaction.inspect record. WorldHover owns it: its update()
// observes the frame's result, its viewItems() asks for the held pose, the view takes the dimming (apply()). The
// Inspect input context and its Back levels are WorldInteraction's (step 7); the wheel reaches it as
// InteractionFrame::wheelNotches (AppInput::mouseWheel()). Main thread only.

#include <optional>

#include <glm/glm.hpp>

#include <rv/app/run_report.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/app/world_interaction.hpp>
#include <rv/render/scene_renderer.hpp>
#include <rv/scene/camera.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/pick_index.hpp>

namespace rv::app {

class WorldInspection {
public:
    /// The frame's interaction ran (`before`: its state as the frame began): the inspections entered, Vedi media and
    /// Informazioni asked for, the quick catalog opened from Inspect (never).
    void observe(const WorldInteraction& interaction, WorldState before, const InteractionResult& result);

    /// The item the outline pass marks: the highlighted one (its outline), else the item in the hand while it is
    /// lifted (the dimming's mask, drawn without an edge).
    [[nodiscard]] static world::PickId markedItem(const WorldInteraction& interaction) noexcept;

    /// The pose of `item` this frame: `pulled` (its pulled-out pose) unless it is the interaction's focused item with a
    /// lift above 0, which goes into the hand in front of `camera` (heldItemTransform() at heldDistance(), limited by
    /// `room`'s cluster of the item), or in VR into `hand` (the pointing controller's grip, I10 step 4); recorded as
    /// the frame's held pose.
    [[nodiscard]] glm::mat4 itemModel(const WorldInteraction& interaction, const CollectionRoom& room,
                                      const world::CollectionItem& item, const glm::mat4& pulled,
                                      const scene::Camera& camera,
                                      const std::optional<scene::RigidPose>& hand = std::nullopt);
    /// Whether the held item's front shows its hero texture this frame (WorldSession knows the hero item).
    void setHeroFront(bool shown) noexcept { m_heroFront = shown; }

    /// The dimming of the frame's view while the item is lifted (inspectDimming()): the share outside the marked item,
    /// the vignette raised, the edge drawn only around a highlighted item (none in Inspect).
    void apply(const WorldInteraction& interaction, gl::RenderView& view);

    void record(InspectRecord& record) const { record = m_record; }

private:
    InspectRecord m_record;
    bool m_heroFront = false;
};

} // namespace rv::app
