#include <rv/app/world_focus.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include <rv/core/math.hpp>

namespace rv::app {
namespace {

using input::Action;

constexpr std::array<Action, 4> kArrows{Action::UiUp, Action::UiDown, Action::UiLeft, Action::UiRight};
constexpr std::array<ui::NavDirection, 4> kDirections{ui::NavDirection::Up, ui::NavDirection::Down,
                                                      ui::NavDirection::Left, ui::NavDirection::Right};

[[nodiscard]] glm::mat4 translation(const glm::vec3& offset) noexcept {
    glm::mat4 transform(1.0f);
    transform[3] = glm::vec4(offset, 1.0f);
    return transform;
}

/// A rotation of `radians` about +Y (right-handed: +X turns towards -Z for a positive angle).
[[nodiscard]] glm::mat4 rotationY(float radians) noexcept {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    glm::mat4 transform(1.0f);
    transform[0] = glm::vec4(c, 0.0f, -s, 0.0f);
    transform[2] = glm::vec4(s, 0.0f, c, 0.0f);
    return transform;
}

/// A rotation of `radians` about +X (right-handed: +Y turns towards +Z for a positive angle).
[[nodiscard]] glm::mat4 rotationX(float radians) noexcept {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    glm::mat4 transform(1.0f);
    transform[1] = glm::vec4(0.0f, c, s, 0.0f);
    transform[2] = glm::vec4(0.0f, -s, c, 0.0f);
    return transform;
}

/// A rotation of `radians` about +Z (right-handed: +X turns towards +Y for a positive angle).
[[nodiscard]] glm::mat4 rotationZ(float radians) noexcept {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    glm::mat4 transform(1.0f);
    transform[0] = glm::vec4(c, s, 0.0f, 0.0f);
    transform[1] = glm::vec4(-s, c, 0.0f, 0.0f);
    return transform;
}

/// The roll about the slot's +Z that stands a spine-out item's cover upright once its quarter turn has brought its
/// front to the viewer (the fix of 2026-10-29): a cartridge on its end (its top face out, its +X up) would show its
/// cover sideways, its picture's top to the left; an item on its side (I6 step 4) is upright already (0).
[[nodiscard]] float uprightRoll(const world::CollectionItem& item) noexcept {
    const glm::mat3 inSlot = glm::mat3(glm::inverse(item.slotFrame) * item.transform);
    const glm::vec3 up = glm::mat3(rotationY(-core::kHalfPi)) * (inSlot * glm::vec3(0.0f, 1.0f, 0.0f));
    if (!std::isfinite(up.x) || !std::isfinite(up.y) || (std::fabs(up.x) < 1e-4f && up.y > 0.0f)) {
        return 0.0f;
    }
    return std::atan2(up.x, up.y);
}

/// The pull in the slot's frame: what moves the item from its place in the slot (item.slotBox) to its pulled pose.
[[nodiscard]] glm::mat4 pullInSlot(const world::CollectionItem& item, float progress, float reach) noexcept {
    const float p = std::isfinite(progress) ? std::clamp(progress, 0.0f, 1.0f) : 0.0f;
    const core::Aabb& box = item.slotBox;
    if (p <= 0.0f || box.isEmpty() || item.inFixture) {
        // A cabinet's screen stays in its bezel (the fix of 2026-10-29): its "pull" is the dynamic copy at its place.
        return glm::mat4(1.0f);
    }
    const glm::vec3 center = box.center();
    if (item.stance == world::ItemStance::FaceOut) {
        const glm::vec3 pivot(center.x, box.min.y, box.max.z);
        return translation(pivot) * rotationX(core::degreesToRadians(kFaceOutTiltDeg) * p) * translation(-pivot);
    }
    // Spine-out: half its width across the row (a) and half its depth into it (b). The row's front stands `reach` past
    // the item's face (box.max.z): the item's back clears it once its centre is 2b + reach + clearance out.
    const glm::vec3 half = box.halfExtents();
    const float a = half.x;
    const float b = half.z;
    const float front = b + std::clamp(std::isfinite(reach) ? reach : 0.0f, 0.0f, kMaxRowReach);
    const float clear = front + b + kPullClearance;
    const float settled = std::max(kPullOutDepthShare * 2.0f * b, front + a + kPullClearance);
    float out = 0.0f;
    float turn = 0.0f;
    float roll = 0.0f;
    if (p <= kPullSlideShare) {
        out = clear * (p / kPullSlideShare);
    } else {
        const float u = (p - kPullSlideShare) / (1.0f - kPullSlideShare);
        // A quarter turn about +Y: its front (the slot's +X) comes round to the slot's +Z, towards the viewer.
        turn = -core::kHalfPi * u;
        // Rolled upright about the slot's +Z meanwhile, clear of its row: a roll about +Z moves nothing along the
        // slot's depth, so the turn's clearance below holds.
        roll = uprightRoll(item) * u;
        // Turned by `turn`, its footprint reaches |sin| a + |cos| b in front of its centre: the centre stays that far
        // plus the clearance in front of the row while it settles to the share of its depth.
        const float turned = a * std::fabs(std::sin(turn)) + b * std::fabs(std::cos(turn));
        out = std::max(front + turned + kPullClearance, clear + (settled - clear) * u);
    }
    return translation(center + glm::vec3(0.0f, 0.0f, out)) * rotationZ(roll) * rotationY(turn) *
           translation(-center);
}

} // namespace

float rowReach(const world::CollectionItem& item, const world::CollectionLayout& layout) noexcept {
    const core::Aabb& box = item.slotBox;
    if (item.stance != world::ItemStance::SpineOut || box.isEmpty()) {
        return 0.0f;
    }
    // In the item's slot frame: what stands across the span its turn sweeps (its centre +- the half diagonal of its
    // footprint), within its height (a board it stands on or under touches it at most: 1 mm off), from behind its back
    // up to kMaxRowReach past its face. An item that rolls upright as it turns (a cartridge on its end, uprightRoll())
    // sweeps the disc of its turned face's half diagonal about its centre, across and up and down: the boards above
    // and below it count then (the fix of 2026-10-29).
    const glm::mat4 toSlot = glm::inverse(item.slotFrame);
    const glm::vec3 half = box.halfExtents();
    const bool rolls = std::fabs(uprightRoll(item)) > 1e-3f;
    const float sweep = std::sqrt(half.y * half.y + half.z * half.z) + kPullClearance;
    const float across = std::max(std::sqrt(half.x * half.x + half.z * half.z) + kPullClearance, rolls ? sweep : 0.0f);
    const float centre = box.center().x;
    constexpr float kTouch = 0.001f;
    const float low = rolls ? box.center().y - sweep : box.min.y + kTouch;
    const float high = rolls ? box.center().y + sweep : box.max.y - kTouch;
    float reach = 0.0f;
    const auto consider = [&](const core::Aabb& local) {
        if (local.isEmpty() || local.max.x <= centre - across || local.min.x >= centre + across ||
            local.max.y <= low || local.min.y >= high || local.max.z <= box.max.z ||
            local.min.z >= box.max.z + kMaxRowReach) {
            return;
        }
        reach = std::max(reach, local.max.z - box.max.z);
    };
    // The region in the room, to pass over what lies elsewhere before turning it into the slot's frame.
    const core::Aabb region =
        core::Aabb::fromMinMax(glm::vec3(centre - across, std::min(low, box.min.y), box.min.z),
                               glm::vec3(centre + across, std::max(high, box.max.y), box.max.z + kMaxRowReach))
            .transformed(item.slotFrame);
    for (const core::Aabb& occluder : layout.occluders) {
        if (region.intersects(occluder)) {
            consider(occluder.transformed(toSlot));
        }
    }
    // Its neighbours stand in its cluster (a module's level): those of another level lie beyond a board, those of
    // another module beyond its side panel, which is an occluder.
    const auto cluster =
        std::find_if(layout.clusters.begin(), layout.clusters.end(),
                     [&](const world::CollectionCluster& candidate) { return candidate.id == item.cluster; });
    if (cluster != layout.clusters.end()) {
        const std::size_t end = std::min<std::size_t>(cluster->firstItem + cluster->itemCount, layout.items.size());
        for (std::size_t index = cluster->firstItem; index < end; ++index) {
            const world::CollectionItem& other = layout.items[index];
            if (other.pickId != item.pickId && region.intersects(other.worldBounds())) {
                consider(other.slotBox.transformed(toSlot * other.slotFrame));
            }
        }
    }
    return std::min(reach, kMaxRowReach);
}

glm::mat4 pulledItemTransform(const world::CollectionItem& item, float progress, float reach) noexcept {
    if (!(progress > 0.0f) || item.inFixture) {
        // In its slot, or part of its fixture (a cabinet's screen): the static pose exactly.
        return item.transform;
    }
    // The item in its slot's frame, moved there, back to the room.
    return item.slotFrame * pullInSlot(item, progress, reach) * glm::inverse(item.slotFrame) * item.transform;
}

core::Aabb itemBounds(const world::CollectionItem& item, float progress, float reach) noexcept {
    return item.slotBox.transformed(item.slotFrame * pullInSlot(item, progress, reach));
}

ItemFace itemFace(const world::CollectionItem& item) noexcept {
    const core::Aabb& box = item.slotBox;
    const glm::vec3 local = box.isEmpty() ? glm::vec3(0.0f) : glm::vec3(box.center().x, box.center().y, box.max.z);
    ItemFace face;
    face.center = glm::vec3(item.slotFrame * glm::vec4(local, 1.0f));
    face.out = scene::frontDirection(item.slotFrame);
    return face;
}

scene::WalkerPose focusPoseFor(const world::CollectionItem& item, const FocusViewer& viewer) noexcept {
    const ItemFace face = itemFace(item);
    const glm::vec3 feet(face.center.x + kFocusDistance * face.out.x, viewer.floorY,
                         face.center.z + kFocusDistance * face.out.z);
    const glm::vec3 eye = feet + glm::vec3(0.0f, viewer.eyeHeight, 0.0f);
    const glm::vec3 target = face.center;
    // Facing the shelf when the target is straight above or below.
    const glm::vec3 back = -face.out;
    const float facingYaw = std::atan2(back.x, -back.z);
    const scene::ViewAngles angles = scene::aimAt(eye, target, viewer.lens, viewer.aspect, viewer.area, facingYaw);
    return scene::WalkerPose{feet, angles.yaw, angles.pitch};
}

Expected<std::unique_ptr<CollectionRoom>> CollectionRoom::build(std::shared_ptr<const world::CollectionLayout> layout) {
    std::unique_ptr<CollectionRoom> room(new CollectionRoom());
    if (!layout) {
        layout = std::make_shared<const world::CollectionLayout>();
    }
    auto navigation = world::SlotNavigation::build(layout->navigation);
    if (!navigation) {
        return propagate(navigation.error());
    }
    room->m_navigation = std::move(*navigation);
    for (std::size_t index = 0; index < layout->items.size(); ++index) {
        room->m_byPick.emplace(layout->items[index].pickId, index);
        room->m_reach.push_back(rowReach(layout->items[index], *layout));
    }
    room->m_layout = std::move(layout);
    return room;
}

const world::CollectionItem* CollectionRoom::find(world::PickId id) const noexcept {
    const auto found = m_byPick.find(id);
    return found == m_byPick.end() ? nullptr : &m_layout->items[found->second];
}

float CollectionRoom::reachOf(world::PickId id) const noexcept {
    const auto found = m_byPick.find(id);
    return found == m_byPick.end() ? 0.0f : m_reach[found->second];
}

std::optional<RoomItem> CollectionRoom::item(world::PickId id) const {
    const world::CollectionItem* found = find(id);
    if (found == nullptr) {
        return std::nullopt;
    }
    return RoomItem{id, found->game, found->stance == world::ItemStance::FaceOut, sectionOf(id), found->inFixture};
}

scene::WalkerPose CollectionRoom::focusPose(world::PickId id) const {
    const world::CollectionItem* found = find(id);
    if (found == nullptr) {
        return scene::WalkerPose{};
    }
    return focusPoseFor(*found, m_viewer);
}

std::optional<world::PickId> CollectionRoom::step(world::PickId from, world::StepDirection direction) const {
    return m_navigation.step(from, direction);
}

FocusStepRepeat::FocusStepRepeat(const ui::NavSettings& settings) noexcept : m_repeater(settings) {}

void FocusStepRepeat::reset() noexcept {
    m_repeater.reset();
    m_pressOrder.fill(0);
    m_suppressHeld = true;
}

input::ActionState FocusStepRepeat::apply(const input::ActionState& actions, double deltaSeconds) {
    bool anyPress = false;
    for (std::size_t index = 0; index < kArrows.size(); ++index) {
        if (actions.pressed(kArrows[index])) {
            m_pressOrder[index] = ++m_pressCounter;
            anyPress = true;
        }
    }
    // The direction held last pressed wins.
    std::optional<std::size_t> held;
    for (std::size_t index = 0; index < kArrows.size(); ++index) {
        if (actions.held(kArrows[index]) && (!held || m_pressOrder[index] > m_pressOrder[*held])) {
            held = index;
        }
    }
    if (m_suppressHeld) {
        if (anyPress || !held) {
            m_suppressHeld = false;
        } else {
            held.reset();
        }
    }
    if (held && actions.pressed(kArrows[*held])) {
        m_repeater.reset(); // a fresh press steps at once, even in the direction already held
    }
    std::optional<std::size_t> step;
    if (const auto repeated = m_repeater.update(held ? std::optional(kDirections[*held]) : std::nullopt,
                                                deltaSeconds)) {
        for (std::size_t index = 0; index < kDirections.size(); ++index) {
            if (kDirections[index] == *repeated) {
                step = index;
            }
        }
    }
    if (!held && !step) {
        // Pressed and released within the frame: one step.
        for (std::size_t index = 0; index < kArrows.size() && !step; ++index) {
            if (actions.pressed(kArrows[index]) && !actions.held(kArrows[index])) {
                step = index;
            }
        }
    }
    input::ActionState result = actions;
    for (std::size_t index = 0; index < kArrows.size(); ++index) {
        const Action arrow = kArrows[index];
        const float value = actions.axis(arrow);
        result.setButton(arrow, actions.held(arrow), step == index, actions.released(arrow));
        result.setAxis(arrow, value);
    }
    return result;
}

core::Ray pickRay(const scene::Camera& camera, PickRaySource source, glm::vec2 cursor, glm::vec2 windowSize) noexcept {
    if (source == PickRaySource::Reticle || !(windowSize.x > 0.0f) || !(windowSize.y > 0.0f) ||
        !std::isfinite(cursor.x) || !std::isfinite(cursor.y)) {
        return camera.rayFromNdc(glm::vec2(0.0f));
    }
    scene::Camera view = camera;
    view.setAspect(windowSize.x / windowSize.y);
    const glm::vec2 ndc(2.0f * cursor.x / windowSize.x - 1.0f, 1.0f - 2.0f * cursor.y / windowSize.y);
    return view.rayFromNdc(glm::clamp(ndc, glm::vec2(-1.0f), glm::vec2(1.0f)));
}

std::optional<glm::vec2> labelAnchor(const core::Aabb& bounds, const glm::mat4& viewProjection, glm::ivec2 size,
                                     float topMargin) noexcept {
    if (bounds.isEmpty() || size.x <= 0 || size.y <= 0 || !core::isFinite(bounds.min) || !core::isFinite(bounds.max)) {
        return std::nullopt;
    }
    glm::vec2 low(std::numeric_limits<float>::max());
    glm::vec2 high(std::numeric_limits<float>::lowest());
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point((corner & 1) != 0 ? bounds.max.x : bounds.min.x,
                              (corner & 2) != 0 ? bounds.max.y : bounds.min.y,
                              (corner & 4) != 0 ? bounds.max.z : bounds.min.z);
        const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
        if (!(clip.w > 1e-6f) || !std::isfinite(clip.x) || !std::isfinite(clip.y)) {
            return std::nullopt; // behind the eye
        }
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        low = glm::min(low, ndc);
        high = glm::max(high, ndc);
    }
    if (high.x < -1.0f || low.x > 1.0f || high.y < -1.0f || low.y > 1.0f) {
        return std::nullopt; // off screen
    }
    const glm::vec2 extent(static_cast<float>(size.x), static_cast<float>(size.y));
    const float x = std::clamp(0.5f * (low.x + high.x), -1.0f, 1.0f);
    const float top = std::min(high.y, 1.0f);
    glm::vec2 anchor(0.5f * (x + 1.0f) * extent.x, 0.5f * (1.0f - top) * extent.y);
    anchor.y = std::max(anchor.y, std::min(topMargin, extent.y));
    return anchor;
}

} // namespace rv::app
