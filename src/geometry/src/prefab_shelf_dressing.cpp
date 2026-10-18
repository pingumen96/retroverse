// Shelf dressing sized to a level (I3 step 1b): the storage boxes, the magazine stacks, the bookends (ADR-0010 §5).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include <rv/core/random.hpp>
#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::translation;

constexpr int kMaxSeed = 2147483647;

Status validate(std::string_view kind, const ShelfDressingParams& p) {
    return firstError({
        checkRange(kind, "width", p.width, 0.1f, 3.0f),
        checkRange(kind, "height", p.height, 0.05f, 1.0f),
        checkRange(kind, "depth", p.depth, 0.05f, 1.0f),
        checkRange(kind, "seed", p.seed, 0, kMaxSeed),
    });
}

/// Uniform in [low, high) from the prop's sequence.
[[nodiscard]] float uniform(core::Random& random, float low, float high) {
    return low + (high - low) * static_cast<float>(random.unit());
}

/// The sequence of one prop: the seed and the kind, so that two kinds with one seed do not vary alike.
[[nodiscard]] core::Random sequenceOf(const ShelfDressingParams& p, std::uint64_t salt) {
    return core::Random(static_cast<std::uint64_t>(p.seed) * 4u + salt);
}

// --- Storage boxes ----------------------------------------------------------------------------------------------

constexpr std::string_view kStorageBox = "storage_box";
/// Width of the cell each box takes along the level.
constexpr float kBoxCell = 0.3f;
constexpr int kMaxBoxes = 8;
constexpr float kLidOverhang = 0.003f;
constexpr float kBoxChamfer = 0.002f;
/// The lids' fronts stand this far behind the space's front.
constexpr float kBoxFrontClear = 0.005f;

ProceduralPrefab assembleStorageBox(const ShelfDressingParams& p) {
    detail::PrefabAssembler out(kStorageBox, {"cardboard", "label"});
    MeshBuilder& cardboard = out.part("cardboard");
    MeshBuilder& label = out.part("label");
    core::Random random = sequenceOf(p, 1);

    const int count = std::clamp(static_cast<int>(std::lround(p.width / kBoxCell)), 1, kMaxBoxes);
    const float cell = p.width / static_cast<float>(count);
    const float front = 0.5f * p.depth - kBoxFrontClear;
    for (int i = 0; i < count; ++i) {
        // The box with its lid takes 78-92% of its cell, 72-90% of the height and 80-100% of the free depth.
        const float outerWidth = cell * uniform(random, 0.78f, 0.92f);
        const float width = outerWidth - 2.0f * kLidOverhang;
        const float height = p.height * uniform(random, 0.72f, 0.9f);
        const float depth = (p.depth - kBoxFrontClear - 2.0f * kLidOverhang - 0.002f) * uniform(random, 0.8f, 1.0f);
        const float lid = std::min(0.025f, 0.25f * height);
        const float body = height - lid;
        const float slack = cell - outerWidth;
        const float x =
            -0.5f * p.width + (static_cast<float>(i) + 0.5f) * cell + 0.5f * slack * uniform(random, -0.6f, 0.6f);
        const float z = front - kLidOverhang - 0.5f * depth;
        cardboard.chamferedBox(glm::vec3(width, body, depth), kBoxChamfer, translation(glm::vec3(x, 0.5f * body, z)),
                               BoxUv::WorldMeters, 0.5f);
        cardboard.chamferedBox(glm::vec3(outerWidth, lid, depth + 2.0f * kLidOverhang), kBoxChamfer,
                               translation(glm::vec3(x, body + 0.5f * lid, z)), BoxUv::WorldMeters, 0.5f);
        // A label card on the front, a little above the middle of the body.
        const glm::vec2 card(0.45f * width, std::min(0.35f * body, 0.05f));
        label.quad(card, translation(glm::vec3(x, 0.55f * body, z + 0.5f * depth + detail::kDecalOffset)));
    }
    return std::move(out).finish();
}

// --- Magazine stacks --------------------------------------------------------------------------------------------

constexpr std::string_view kMagazineStack = "magazine_stack";
/// A magazine lying flat: about 28 x 21 cm.
constexpr float kMagazineWidth = 0.28f;
constexpr float kMagazineDepth = 0.21f;
constexpr int kMaxStacks = 4;
/// Room kept around a stack for the turns and offsets of its magazines.
constexpr float kStackRoom = 0.013f;
constexpr float kMaxTurnDeg = 3.0f;
constexpr float kMaxOffset = 0.004f;

ProceduralPrefab assembleMagazineStack(const ShelfDressingParams& p) {
    detail::PrefabAssembler out(kMagazineStack, {"paper", "cover"});
    MeshBuilder& paper = out.part("paper");
    MeshBuilder& cover = out.part("cover");
    core::Random random = sequenceOf(p, 2);

    const float depth = std::min(kMagazineDepth, p.depth - 2.0f * kStackRoom);
    const float nominalWidth = std::min(kMagazineWidth, depth * (kMagazineWidth / kMagazineDepth));
    const int count = std::clamp(
        static_cast<int>(std::floor((p.width + 0.04f) / (nominalWidth + 2.0f * kStackRoom + 0.04f))), 1, kMaxStacks);
    const float cell = p.width / static_cast<float>(count);
    const float width = std::min(nominalWidth, cell - 2.0f * kStackRoom);
    for (int i = 0; i < count; ++i) {
        const float slack = cell - width - 2.0f * kStackRoom;
        const float cx =
            -0.5f * p.width + (static_cast<float>(i) + 0.5f) * cell + 0.5f * slack * uniform(random, -0.7f, 0.7f);
        const int wanted = static_cast<int>(random.between(5, 12));
        float y = 0.0f;
        glm::mat4 topFrame(1.0f);
        for (int m = 0; m < wanted; ++m) {
            const float thickness = uniform(random, 0.004f, 0.007f);
            if (y + thickness > p.height - 0.002f) {
                break;
            }
            const float turn = glm::radians(uniform(random, -kMaxTurnDeg, kMaxTurnDeg));
            const glm::vec3 center(cx + uniform(random, -kMaxOffset, kMaxOffset), y + 0.5f * thickness,
                                   uniform(random, -kMaxOffset, kMaxOffset));
            const glm::mat4 frame = glm::rotate(translation(center), turn, glm::vec3(0.0f, 1.0f, 0.0f));
            paper.box(glm::vec3(width, thickness, depth), frame, BoxUv::PerFace);
            topFrame = glm::translate(frame, glm::vec3(0.0f, 0.5f * thickness + detail::kDecalOffset, 0.0f));
            y += thickness;
        }
        if (y > 0.0f) {
            // The cover of the top magazine, its picture upright seen from the front (image top towards -Z).
            const glm::mat4 faceUp = glm::rotate(topFrame, glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
            cover.quad(glm::vec2(width - 0.004f, depth - 0.004f), faceUp);
        }
    }
    return std::move(out).finish();
}

// --- Bookends ---------------------------------------------------------------------------------------------------

constexpr std::string_view kBookends = "bookends";
constexpr float kBookendWidth = 0.04f;
constexpr float kBookChamfer = 0.0015f;
constexpr float kBookendChamfer = 0.003f;
/// The row's front stands this far behind the space's front.
constexpr float kRowFrontClear = 0.015f;

ProceduralPrefab assembleBookends(const ShelfDressingParams& p) {
    detail::PrefabAssembler out(kBookends, {"metal", "book", "book_alt"});
    core::Random random = sequenceOf(p, 3);

    const float bookendWidth = std::min(kBookendWidth, 0.2f * p.width);
    const float tallest = std::min(p.height - 0.005f, 0.24f);
    const float deepest = std::min(p.depth - kRowFrontClear - 0.002f, 0.22f);
    const float front = 0.5f * p.depth - kRowFrontClear;
    // The books: a row of 50-75% of the width between the bookends, at least one book.
    const float rowTarget = p.width * uniform(random, 0.5f, 0.75f) - 2.0f * bookendWidth;
    const float maxRow = p.width - 2.0f * bookendWidth;
    struct Book {
        float thickness, height, depth;
        bool alt;
    };
    std::vector<Book> books;
    float row = 0.0f;
    bool alt = random.below(2) == 1;
    int run = static_cast<int>(random.between(1, 3));
    while (true) {
        const float thickness = std::min(uniform(random, 0.018f, 0.045f), maxRow);
        if (!books.empty() && (row + thickness > rowTarget || row + thickness > maxRow)) {
            break;
        }
        books.push_back(
            Book{thickness, tallest * uniform(random, 0.72f, 0.97f), deepest * uniform(random, 0.6f, 0.85f), alt});
        row += thickness;
        if (--run == 0) {
            alt = !alt;
            run = static_cast<int>(random.between(1, 3));
        }
    }
    const float total = row + 2.0f * bookendWidth;
    const float x0 = -0.5f * total + 0.5f * (p.width - total) * uniform(random, -0.7f, 0.7f);

    MeshBuilder& metal = out.part("metal");
    const float bookendHeight = std::min(0.13f, 0.75f * p.height);
    const float bookendDepth = std::min(0.12f, 0.6f * deepest);
    for (const float x : {x0 + 0.5f * bookendWidth, x0 + total - 0.5f * bookendWidth}) {
        metal.chamferedBox(glm::vec3(bookendWidth, bookendHeight, bookendDepth), kBookendChamfer,
                           translation(glm::vec3(x, 0.5f * bookendHeight, front - 0.5f * bookendDepth)));
    }
    float x = x0 + bookendWidth;
    for (const Book& book : books) {
        out.part(book.alt ? "book_alt" : "book")
            .chamferedBox(
                glm::vec3(book.thickness, book.height, book.depth), kBookChamfer,
                translation(glm::vec3(x + 0.5f * book.thickness, 0.5f * book.height, front - 0.5f * book.depth)),
                BoxUv::PerFace);
        x += book.thickness;
    }
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildStorageBox(const ShelfDressingParams& params) {
    if (Status status = validate(kStorageBox, params); !status) {
        return propagate(status.error());
    }
    return assembleStorageBox(params);
}

Expected<ProceduralPrefab> buildMagazineStack(const ShelfDressingParams& params) {
    if (Status status = validate(kMagazineStack, params); !status) {
        return propagate(status.error());
    }
    return assembleMagazineStack(params);
}

Expected<ProceduralPrefab> buildBookends(const ShelfDressingParams& params) {
    if (Status status = validate(kBookends, params); !status) {
        return propagate(status.error());
    }
    return assembleBookends(params);
}

} // namespace rv::geometry
