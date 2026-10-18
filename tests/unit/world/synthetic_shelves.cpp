#include "synthetic_shelves.hpp"

#include <algorithm>
#include <stdexcept>

#include <glm/gtc/matrix_transform.hpp>

namespace rv::test {

namespace {

[[nodiscard]] core::Aabb worldBox(const glm::mat4& runFrame, const glm::vec3& min, const glm::vec3& max) {
    return core::Aabb::fromMinMax(min, max).transformed(runFrame);
}

[[nodiscard]] bool listed(const std::vector<int>& levels, int level) {
    return std::find(levels.begin(), levels.end(), level) != levels.end();
}

[[nodiscard]] bool listed(const std::vector<std::pair<int, int>>& slots, int level, int slot) {
    return std::find(slots.begin(), slots.end(), std::pair{level, slot}) != slots.end();
}

} // namespace

const SyntheticItem& SyntheticShelves::item(world::PickId id) const {
    for (const SyntheticItem& candidate : items) {
        if (candidate.id == id) {
            return candidate;
        }
    }
    throw std::out_of_range("no synthetic item with this id");
}

const SyntheticItem* SyntheticShelves::at(int module, int level, int slot) const {
    for (const SyntheticItem& candidate : items) {
        if (candidate.module == module && candidate.level == level && candidate.slot == slot) {
            return &candidate;
        }
    }
    return nullptr;
}

SyntheticShelves buildShelves(std::span<const ShelfModuleSpec> modules, const glm::mat4& runFrame,
                              world::PickId firstId) {
    using namespace shelf;
    SyntheticShelves shelves;
    world::PickId nextId = firstId;
    const float halfWidth = 0.5f * kWidth;
    const float halfDepth = 0.5f * kDepth;
    const float innerLeft = -halfWidth + kSideThickness;
    const float slotFront = halfDepth - kFrontInset;
    const glm::mat3 rotation(runFrame);
    for (std::size_t m = 0; m < modules.size(); ++m) {
        const ShelfModuleSpec& spec = modules[m];
        const float x0 = static_cast<float>(m) * kModuleSpacing;
        std::vector<core::Aabb>& boards = shelves.boards.emplace_back();
        // The solid pieces: base, sides, back panel, a board under each level and one over the top level.
        shelves.occluders.push_back(
            worldBox(runFrame, {x0 - halfWidth, 0.0f, -halfDepth}, {x0 + halfWidth, kBaseTop, halfDepth}));
        shelves.occluders.push_back(worldBox(runFrame, {x0 - halfWidth, 0.0f, -halfDepth},
                                             {x0 - halfWidth + kSideThickness, kHeight, halfDepth}));
        shelves.occluders.push_back(worldBox(runFrame, {x0 + halfWidth - kSideThickness, 0.0f, -halfDepth},
                                             {x0 + halfWidth, kHeight, halfDepth}));
        shelves.occluders.push_back(
            worldBox(runFrame, {x0 - halfWidth, 0.0f, -halfDepth}, {x0 + halfWidth, kHeight, -halfDepth + kBackThickness}));
        for (int k = 0; k <= spec.levels; ++k) {
            const float top = kFirstBoardTop + kLevelPitch * static_cast<float>(k);
            const core::Aabb board = worldBox(runFrame, {x0 - halfWidth + kSideThickness, top - kBoardThickness, -halfDepth},
                                              {x0 + halfWidth - kSideThickness, top, halfDepth});
            shelves.occluders.push_back(board);
            boards.push_back(board);
        }

        world::NavFixture& fixture = shelves.run.fixtures.emplace_back();
        for (int level = 0; level < spec.levels; ++level) {
            const float boardTop = kFirstBoardTop + kLevelPitch * static_cast<float>(level);
            const bool faceOut = spec.faceOutLevel == level;
            const bool dressed = listed(spec.dressedLevels, level);
            const int count = faceOut ? kFaceSlots : spec.slotsPerLevel;
            const glm::vec3 size = faceOut ? glm::vec3(kFaceWidth, kFaceHeight, kFaceDepth)
                                           : glm::vec3(kSpineWidth, kSpineHeight, kSpineDepth);
            const float pitch = faceOut ? kFaceWidth + kFaceGap : kSpinePitch;
            const float margin = faceOut ? kFaceMargin : kSlotMargin;
            world::NavLevel& navLevel = fixture.levels.emplace_back();
            navLevel.height = boardTop + 0.5f * size.y;
            for (int slot = 0; slot < count; ++slot) {
                const glm::vec3 center(x0 + innerLeft + margin + 0.5f * size.x + pitch * static_cast<float>(slot),
                                       boardTop + 0.5f * size.y, slotFront - 0.5f * size.z);
                world::NavSlot& navSlot = navLevel.slots.emplace_back();
                navSlot.along = center.x;
                if (dressed || listed(spec.emptySlots, level, slot)) {
                    continue;
                }
                SyntheticItem item;
                item.id = nextId++;
                item.module = static_cast<int>(m);
                item.level = level;
                item.slot = slot;
                item.faceOut = faceOut;
                item.frame = runFrame * glm::translate(glm::mat4(1.0f), center);
                item.box = core::Aabb::fromCenterHalfExtents(glm::vec3(0.0f), 0.5f * size);
                item.faceCenter = glm::vec3(item.frame * glm::vec4(0.0f, 0.0f, 0.5f * size.z, 1.0f));
                item.right = glm::normalize(rotation * glm::vec3(1.0f, 0.0f, 0.0f));
                item.up = glm::normalize(rotation * glm::vec3(0.0f, 1.0f, 0.0f));
                item.out = glm::normalize(rotation * glm::vec3(0.0f, 0.0f, 1.0f));
                navSlot.item = item.id;
                shelves.targets.push_back(world::PickTarget{
                    item.id, static_cast<std::uint32_t>(m * 100 + static_cast<std::size_t>(level)), item.frame,
                    world::faceTargetBox(item.box)});
                shelves.items.push_back(item);
            }
        }
    }
    return shelves;
}

core::Ray rayTowards(const glm::vec3& eye, const glm::vec3& point) {
    return core::Ray{eye, glm::normalize(point - eye)};
}

} // namespace rv::test
