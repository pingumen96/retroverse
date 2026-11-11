// I12 step 4's tall decoration (ADR-0021 §1): the pieces that fill a side wall between the modules up towards the rail:
// a bookcase of books, binders and magazine files, a rack of records or tapes, a pegboard of game pads, a coat stand.
// Every one of original design; nothing of them reads as a game (plain spines, no fronts, no titles).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include <rv/core/math.hpp>
#include <rv/core/random.hpp>
#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::outOfRange;
using detail::translation;

constexpr int kMaxSeed = 2147483647;
constexpr BoxUv kGrain = BoxUv::GrainMeters;
constexpr float kWoodTile = detail::kWoodMetersPerTile;

[[nodiscard]] float uniform(core::Random& random, float low, float high) {
    return low + (high - low) * static_cast<float>(random.unit());
}

// --- The carcass of an open unit (bookcase, rack) ---------------------------------------------------------------

/// The boards of an open unit, metres.
struct Carcass {
    float side = 0.018f;
    float top = 0.025f;
    float plinth = 0.06f;
    float board = 0.018f;
    float back = 0.008f;
};

/// Where a unit's levels stand: each level's floor (its board's top), its clear height, the inside's x range and the
/// inside's depth from the back panel's face to the front.
struct Levels {
    std::vector<float> floors;
    float clear = 0.0f;
    float left = 0.0f;
    float right = 0.0f;
    float backZ = 0.0f;
    float frontZ = 0.0f;
};

[[nodiscard]] float clearOf(float height, int levels, const Carcass& c) {
    return (height - c.plinth - c.top - static_cast<float>(levels - 1) * c.board) / static_cast<float>(levels);
}

[[nodiscard]] Levels levelsOf(float width, float height, float depth, int levels, const Carcass& c) {
    Levels out;
    out.clear = clearOf(height, levels, c);
    for (int i = 0; i < levels; ++i) {
        out.floors.push_back(c.plinth + static_cast<float>(i) * (out.clear + c.board));
    }
    out.left = -0.5f * width + c.side;
    out.right = 0.5f * width - c.side;
    out.backZ = -0.5f * depth + c.back;
    out.frontZ = 0.5f * depth;
    return out;
}

/// The sides, the top, the plinth (its front 2 cm back: a toe kick), the boards between the levels and the back panel.
void buildCarcass(detail::PrefabAssembler& out, float width, float height, float depth, const Levels& levels,
                  const Carcass& c) {
    MeshBuilder& wood = out.part("wood_primary");
    for (const float side : {-1.0f, 1.0f}) {
        wood.box(glm::vec3(c.side, height, depth),
                 translation(glm::vec3(side * (0.5f * width - 0.5f * c.side), 0.5f * height, 0.0f)), kGrain, kWoodTile);
    }
    const float inner = width - 2.0f * c.side;
    wood.chamferedBox(glm::vec3(width, c.top, depth), 0.003f, translation(glm::vec3(0.0f, height - 0.5f * c.top, 0.0f)),
                      kGrain, kWoodTile);
    const float toe = 0.02f;
    wood.box(glm::vec3(inner, c.plinth, depth - toe - c.back),
             translation(glm::vec3(0.0f, 0.5f * c.plinth, 0.5f * (c.back - toe))), kGrain, kWoodTile);
    for (std::size_t i = 1; i < levels.floors.size(); ++i) {
        wood.box(glm::vec3(inner, c.board, depth - c.back),
                 translation(glm::vec3(0.0f, levels.floors[i] - 0.5f * c.board, 0.5f * c.back)), kGrain, kWoodTile);
    }
    out.part("wood_secondary")
        .box(glm::vec3(inner, height - c.top, c.back),
             translation(glm::vec3(0.0f, 0.5f * (height - c.top), -0.5f * depth + 0.5f * c.back)), kGrain, kWoodTile);
}

/// Picks the next of `count` roles in runs of one to three, never the same run twice in a row.
class RoleRuns {
public:
    RoleRuns(core::Random& random, int count) : m_random(random), m_count(count) {
        m_role = static_cast<int>(m_random.below(static_cast<std::uint64_t>(m_count)));
        m_left = static_cast<int>(m_random.between(1, 3));
    }
    int next() {
        if (m_left == 0) {
            const int step = 1 + static_cast<int>(m_random.below(static_cast<std::uint64_t>(m_count - 1)));
            m_role = (m_role + step) % m_count;
            m_left = static_cast<int>(m_random.between(1, 3));
        }
        --m_left;
        return m_role;
    }

private:
    core::Random& m_random;
    int m_count = 1;
    int m_role = 0;
    int m_left = 0;
};

// --- bookcase -------------------------------------------------------------------------------------------------------

constexpr std::string_view kBookcase = "bookcase";
constexpr std::array<std::string_view, 3> kSpineRoles{"spine_a", "spine_b", "spine_c"};
constexpr float kMinBookcaseClear = 0.22f;
/// The spines stand this far behind the boards' front, give or take.
constexpr float kSpineInset = 0.012f;

Status validate(const BookcaseParams& p) {
    if (Status status = firstError(
            {checkRange(kBookcase, "width", p.width, 0.4f, 1.2f), checkRange(kBookcase, "height", p.height, 0.9f, 2.6f),
             checkRange(kBookcase, "depth", p.depth, 0.2f, 0.5f), checkRange(kBookcase, "levels", p.levels, 2, 8),
             checkRange(kBookcase, "fill", p.fill, 0.0f, 1.0f), checkRange(kBookcase, "seed", p.seed, 0, kMaxSeed)});
        !status) {
        return status;
    }
    if (clearOf(p.height, p.levels, Carcass{}) < kMinBookcaseClear) {
        return outOfRange(kBookcase, "levels", "the bookcase is too low for its levels (0.22 m clear each)");
    }
    return ok();
}

/// One level's books from `x` to about `end`, the last one or two leaning on the row when there is room.
void fillBooks(detail::PrefabAssembler& out, core::Random& random, const Levels& levels, float floor, float end) {
    const float tallest = std::min(levels.clear - 0.012f, 0.32f);
    const float deepest = std::min(levels.frontZ - levels.backZ - kSpineInset - 0.004f, 0.25f);
    RoleRuns roles(random, 3);
    float x = levels.left + uniform(random, 0.0f, 0.01f);
    float lastHeight = 0.0f;
    while (true) {
        const float thickness = uniform(random, 0.012f, 0.035f);
        if (x + thickness > end) {
            break;
        }
        const float height = tallest * uniform(random, 0.62f, 0.97f);
        const float depth = deepest * uniform(random, 0.7f, 0.97f);
        const float front = levels.frontZ - kSpineInset - uniform(random, 0.0f, 0.006f);
        out.part(kSpineRoles[static_cast<std::size_t>(roles.next())])
            .box(glm::vec3(thickness, height, depth),
                 translation(glm::vec3(x + 0.5f * thickness, floor + 0.5f * height, front - 0.5f * depth)));
        x += thickness;
        lastHeight = height;
        // Now and then a narrow gap in the row.
        if (random.below(20) == 0) {
            x += uniform(random, 0.004f, 0.012f);
        }
    }
    // A book leaning on the row's last one, its top against that book's side, where it fits.
    if (lastHeight <= 0.0f || random.below(10) >= 6) {
        return;
    }
    const float angle = glm::radians(uniform(random, 8.0f, 16.0f));
    const float thickness = uniform(random, 0.015f, 0.03f);
    const float height = lastHeight * uniform(random, 0.85f, 0.98f) / std::cos(angle);
    const float foot = x + height * std::sin(angle);
    if (foot + thickness * std::cos(angle) > levels.right - 0.004f || height > tallest) {
        return;
    }
    const float depth = deepest * uniform(random, 0.75f, 0.95f);
    const float front = levels.frontZ - kSpineInset;
    const glm::vec3 centre(foot + 0.5f * thickness * std::cos(angle) - 0.5f * height * std::sin(angle),
                           floor + 0.5f * thickness * std::sin(angle) + 0.5f * height * std::cos(angle),
                           front - 0.5f * depth);
    out.part(kSpineRoles[static_cast<std::size_t>(roles.next())])
        .box(glm::vec3(thickness, height, depth), glm::rotate(translation(centre), angle, glm::vec3(0.0f, 0.0f, 1.0f)));
}

/// One level's binders: a run of one colour with now and then another, a paper strip on each spine.
void fillBinders(detail::PrefabAssembler& out, core::Random& random, const Levels& levels, float floor, float end) {
    const float height = std::min(levels.clear - 0.012f, 0.3f) * uniform(random, 0.96f, 1.0f);
    const float depth = std::min(levels.frontZ - levels.backZ - kSpineInset - 0.004f, 0.26f);
    const int main = static_cast<int>(random.below(3));
    float x = levels.left + uniform(random, 0.0f, 0.01f);
    const float front = levels.frontZ - kSpineInset;
    while (true) {
        const float thickness = uniform(random, 0.05f, 0.07f);
        if (x + thickness > end) {
            break;
        }
        const int role = random.below(5) == 0 ? (main + 1) % 3 : main;
        out.part(kSpineRoles[static_cast<std::size_t>(role)])
            .box(glm::vec3(thickness, height, depth),
                 translation(glm::vec3(x + 0.5f * thickness, floor + 0.5f * height, front - 0.5f * depth)));
        out.part("paper").quad(
            glm::vec2(0.62f * thickness, 0.14f * height),
            translation(glm::vec3(x + 0.5f * thickness, floor + 0.64f * height, front + detail::kDecalOffset)));
        x += thickness + 0.002f;
    }
}

/// One level's magazine files: an upright box, its front cut low (a high back, a low front), a strip on the front.
void fillMagazineFiles(detail::PrefabAssembler& out, core::Random& random, const Levels& levels, float floor,
                       float end) {
    const float height = std::min(levels.clear - 0.012f, 0.31f);
    const float depth = std::min(levels.frontZ - levels.backZ - kSpineInset - 0.004f, 0.24f);
    RoleRuns roles(random, 3);
    float x = levels.left + uniform(random, 0.0f, 0.01f);
    const float front = levels.frontZ - kSpineInset;
    while (true) {
        const float thickness = uniform(random, 0.075f, 0.1f);
        if (x + thickness > end) {
            break;
        }
        const float high = height * uniform(random, 0.92f, 1.0f);
        const float low = 0.55f * high;
        MeshBuilder& file = out.part(kSpineRoles[static_cast<std::size_t>(roles.next())]);
        const float cx = x + 0.5f * thickness;
        file.box(glm::vec3(thickness, high, 0.45f * depth),
                 translation(glm::vec3(cx, floor + 0.5f * high, front - depth + 0.225f * depth)));
        file.box(glm::vec3(thickness, low, 0.55f * depth),
                 translation(glm::vec3(cx, floor + 0.5f * low, front - 0.275f * depth)));
        out.part("paper").quad(glm::vec2(0.6f * thickness, 0.1f * low),
                               translation(glm::vec3(cx, floor + 0.62f * low, front + detail::kDecalOffset)));
        x += thickness + 0.003f;
    }
}

ProceduralPrefab assemble(const BookcaseParams& p) {
    detail::PrefabAssembler out(kBookcase,
                                {"wood_primary", "wood_secondary", "spine_a", "spine_b", "spine_c", "paper"});
    const Carcass carcass;
    const Levels levels = levelsOf(p.width, p.height, p.depth, p.levels, carcass);
    buildCarcass(out, p.width, p.height, p.depth, levels, carcass);
    core::Random random(static_cast<std::uint64_t>(p.seed) * 8u + 5u);
    const float inner = levels.right - levels.left;
    for (const float floor : levels.floors) {
        const float share = std::clamp(p.fill * uniform(random, 0.85f, 1.1f), 0.0f, 1.0f);
        const float end = levels.left + share * inner;
        BookcaseFill contents = p.contents;
        if (contents == BookcaseFill::Mixed) {
            const std::uint64_t draw = random.below(10);
            contents = draw < 6 ? BookcaseFill::Books : draw < 8 ? BookcaseFill::Binders : BookcaseFill::Magazines;
        }
        switch (contents) {
        case BookcaseFill::Binders:
            fillBinders(out, random, levels, floor, end);
            break;
        case BookcaseFill::Magazines:
            fillMagazineFiles(out, random, levels, floor, end);
            break;
        case BookcaseFill::Books:
        case BookcaseFill::Mixed:
            fillBooks(out, random, levels, floor, end);
            break;
        }
    }
    return std::move(out).finish();
}

// --- record_rack ----------------------------------------------------------------------------------------------------

constexpr std::string_view kRack = "record_rack";
constexpr std::array<std::string_view, 3> kSleeveRoles{"sleeve_a", "sleeve_b", "sleeve_c"};
constexpr float kSleeve = 0.31f;
constexpr glm::vec3 kTape{0.025f, 0.188f, 0.103f};

/// The rack's carcass: slimmer boards than a bookcase's.
[[nodiscard]] Carcass rackCarcass() {
    Carcass c;
    c.side = 0.016f;
    c.top = 0.02f;
    c.plinth = 0.04f;
    c.board = 0.016f;
    c.back = 0.006f;
    return c;
}

Status validate(const RecordRackParams& p) {
    if (Status status = firstError(
            {checkRange(kRack, "width", p.width, 0.4f, 1.0f), checkRange(kRack, "height", p.height, 0.6f, 2.2f),
             checkRange(kRack, "depth", p.depth, 0.2f, 0.45f), checkRange(kRack, "levels", p.levels, 2, 8),
             checkRange(kRack, "fill", p.fill, 0.0f, 1.0f), checkRange(kRack, "seed", p.seed, 0, kMaxSeed)});
        !status) {
        return status;
    }
    const Carcass carcass = rackCarcass();
    const float clear = clearOf(p.height, p.levels, carcass);
    const float inside = p.depth - carcass.back;
    if (p.media == RackMedia::Records && (clear < kSleeve + 0.01f || inside < kSleeve + 0.005f)) {
        return outOfRange(kRack, "levels", "records need levels 0.32 m clear and 0.315 m deep inside");
    }
    if (p.media == RackMedia::Tapes && (clear < kTape.y + 0.012f || inside < kTape.z + 0.005f)) {
        return outOfRange(kRack, "levels", "tapes need levels 0.2 m clear and 0.108 m deep inside");
    }
    return ok();
}

ProceduralPrefab assemble(const RecordRackParams& p) {
    detail::PrefabAssembler out(kRack, {"wood_primary", "wood_secondary", "sleeve_a", "sleeve_b", "sleeve_c"});
    const Carcass carcass = rackCarcass();
    const Levels levels = levelsOf(p.width, p.height, p.depth, p.levels, carcass);
    buildCarcass(out, p.width, p.height, p.depth, levels, carcass);
    core::Random random(static_cast<std::uint64_t>(p.seed) * 8u + 6u);
    const float inner = levels.right - levels.left;
    for (const float floor : levels.floors) {
        const float share = std::clamp(p.fill * uniform(random, 0.85f, 1.1f), 0.0f, 1.0f);
        const float end = levels.left + share * inner;
        float x = levels.left + uniform(random, 0.0f, 0.008f);
        if (p.media == RackMedia::Records) {
            // Sleeves of two tones in runs, now and then the third.
            RoleRuns roles(random, 2);
            const float front = levels.frontZ - 0.004f;
            while (true) {
                const float thickness = uniform(random, 0.003f, 0.006f);
                if (x + thickness > end) {
                    break;
                }
                const int role = random.below(12) == 0 ? 2 : roles.next();
                const float size = kSleeve - uniform(random, 0.0f, 0.004f);
                out.part(kSleeveRoles[static_cast<std::size_t>(role)])
                    .box(glm::vec3(thickness, size, size),
                         translation(glm::vec3(x + 0.5f * thickness, floor + 0.5f * size, front - 0.5f * size)));
                x += thickness;
            }
        } else {
            // Tapes in their cases (one tone), each spine with a label of another.
            const int body = static_cast<int>(random.below(2));
            const float front = levels.frontZ - 0.01f;
            while (x + kTape.x <= end) {
                const glm::vec3 centre(x + 0.5f * kTape.x, floor + 0.5f * kTape.y, front - 0.5f * kTape.z);
                out.part(kSleeveRoles[static_cast<std::size_t>(body)]).box(kTape, translation(centre));
                const int label = random.below(4) == 0 ? 2 : 1 - body;
                out.part(kSleeveRoles[static_cast<std::size_t>(label)])
                    .quad(glm::vec2(0.7f * kTape.x, 0.78f * kTape.y),
                          translation(glm::vec3(centre.x, centre.y, front + detail::kDecalOffset)));
                x += kTape.x + 0.001f;
            }
        }
    }
    return std::move(out).finish();
}

// --- pegboard -------------------------------------------------------------------------------------------------------

constexpr std::string_view kPegboard = "pegboard";
constexpr float kPegTrim = 0.022f;
constexpr float kPegBoard = 0.006f;
/// The board stands this far off the wall (its battens behind it).
constexpr float kPegStandoff = 0.012f;
constexpr float kHolePitch = 0.025f;
constexpr float kHole = 0.005f;
constexpr float kHookLength = 0.035f;
/// A pad's cell on the board: its width plus this, and its height plus this.
constexpr float kPadMargin = 0.05f;
/// A hung pad's back stands this far in front of the board (on its hooks).
constexpr float kPadOff = 0.004f;

/// A pad as a pegboard hangs it: buildPad()'s body and buttons (its cable left out) and their bounds.
struct HungPad {
    std::vector<const PrefabPart*> parts;
    core::Aabb bounds;
};

/// The pad of `style` as buildPad() makes it.
[[nodiscard]] Expected<ProceduralPrefab> padOf(PadStyle style) {
    PadParams pad;
    pad.style = style;
    return buildPad(pad);
}

[[nodiscard]] HungPad hungPad(const ProceduralPrefab& pad) {
    HungPad hung;
    bool first = true;
    for (const PrefabPart& part : pad.parts) {
        if (part.materialRole == "plastic_body" || part.materialRole == "plastic_dark") {
            hung.parts.push_back(&part);
            hung.bounds = first ? part.mesh.bounds : hung.bounds.merged(part.mesh.bounds);
            first = false;
        }
    }
    return hung;
}

/// The pads a board takes: columns and rows of cells.
struct PadGrid {
    int columns = 0;
    int rows = 0;
    glm::vec2 cell{0.0f};
};

[[nodiscard]] PadGrid padGridOf(const PegboardParams& p, const core::Aabb& pad) {
    // The pad hangs face out: its width across, its depth (front to back) up and down the board.
    const glm::vec2 size(pad.max.x - pad.min.x, pad.max.z - pad.min.z);
    const glm::vec2 inside(p.width - 2.0f * kPegTrim, p.height - 2.0f * kPegTrim);
    PadGrid grid;
    grid.cell = size + glm::vec2(kPadMargin);
    grid.columns = std::max(0, static_cast<int>(std::floor(inside.x / grid.cell.x)));
    grid.rows = std::min(3, std::max(0, static_cast<int>(std::floor(inside.y / grid.cell.y))));
    return grid;
}

Status validate(const PegboardParams& p) {
    if (Status status = firstError(
            {checkRange(kPegboard, "width", p.width, 0.4f, 1.2f), checkRange(kPegboard, "height", p.height, 0.4f, 1.6f),
             checkRange(kPegboard, "depth", p.depth, 0.03f, 0.2f), checkRange(kPegboard, "pads", p.pads, 0, 10),
             checkRange(kPegboard, "seed", p.seed, 0, kMaxSeed)});
        !status) {
        return status;
    }
    if (p.pads == 0) {
        return ok();
    }
    auto pad = padOf(p.style);
    if (!pad) {
        return propagate(pad.error());
    }
    const core::Aabb bounds = hungPad(*pad).bounds;
    const PadGrid grid = padGridOf(p, bounds);
    if (p.pads > grid.columns * grid.rows) {
        return outOfRange(kPegboard, "pads",
                          "the board holds " + std::to_string(grid.columns * grid.rows) + " pads of that style");
    }
    // The pad's height (its thickness, hung face out) in front of the board.
    if (kPegStandoff + kPegBoard + kPadOff + bounds.max.y > p.depth) {
        return outOfRange(kPegboard, "depth", "the pads of that style stand out further than the depth");
    }
    return ok();
}

ProceduralPrefab assemble(const PegboardParams& p) {
    detail::PrefabAssembler out(kPegboard, {"board", "trim", "holes", "metal", "plastic_body", "plastic_dark"});
    const glm::vec2 inside(p.width - 2.0f * kPegTrim, p.height - 2.0f * kPegTrim);
    const float boardFront = kPegStandoff + kPegBoard;
    out.part("board").box(glm::vec3(inside, kPegBoard),
                          translation(glm::vec3(0.0f, 0.0f, kPegStandoff + 0.5f * kPegBoard)));
    // The frame around the board, from the wall to a little proud of it.
    const float trimDepth = boardFront + 0.006f;
    MeshBuilder& trim = out.part("trim");
    for (const float side : {-1.0f, 1.0f}) {
        trim.chamferedBox(glm::vec3(p.width, kPegTrim, trimDepth), 0.002f,
                          translation(glm::vec3(0.0f, side * (0.5f * p.height - 0.5f * kPegTrim), 0.5f * trimDepth)),
                          kGrain, kWoodTile);
        trim.chamferedBox(glm::vec3(kPegTrim, inside.y, trimDepth), 0.002f,
                          translation(glm::vec3(side * (0.5f * p.width - 0.5f * kPegTrim), 0.0f, 0.5f * trimDepth)),
                          kGrain, kWoodTile);
    }
    // The holes, dark squares in a grid centred on the board.
    const int across = static_cast<int>(std::floor((inside.x - kHolePitch) / kHolePitch)) + 1;
    const int down = static_cast<int>(std::floor((inside.y - kHolePitch) / kHolePitch)) + 1;
    MeshBuilder& holes = out.part("holes");
    for (int row = 0; row < down; ++row) {
        for (int column = 0; column < across; ++column) {
            const float x = (static_cast<float>(column) - 0.5f * static_cast<float>(across - 1)) * kHolePitch;
            const float y = (static_cast<float>(row) - 0.5f * static_cast<float>(down - 1)) * kHolePitch;
            holes.quad(glm::vec2(kHole), translation(glm::vec3(x, y, boardFront + detail::kDecalOffset)));
        }
    }
    if (p.pads == 0) {
        return std::move(out).finish();
    }
    // The pads in cells of the grid the seed picks, each on two hooks over its top edge.
    auto built = padOf(p.style);
    if (!built) {
        return std::move(out).finish(); // validate() built it already
    }
    const HungPad pad = hungPad(*built);
    const core::Aabb& bounds = pad.bounds;
    const PadGrid grid = padGridOf(p, bounds);
    std::vector<int> cells;
    for (int i = 0; i < grid.columns * grid.rows; ++i) {
        cells.push_back(i);
    }
    core::Random random(static_cast<std::uint64_t>(p.seed) * 8u + 7u);
    for (std::size_t i = cells.size(); i > 1; --i) {
        std::swap(cells[i - 1], cells[static_cast<std::size_t>(random.below(i))]);
    }
    cells.resize(static_cast<std::size_t>(p.pads));
    std::sort(cells.begin(), cells.end());
    const glm::vec2 span(static_cast<float>(grid.columns) * grid.cell.x, static_cast<float>(grid.rows) * grid.cell.y);
    MeshBuilder& metal = out.part("metal");
    // Hung face out: the pad's top (+Y) turned to face +Z, its front (+Z) down.
    const glm::mat4 faceOut = glm::rotate(glm::mat4(1.0f), 0.5f * core::kPi, glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::vec2 padCentre(0.5f * (bounds.min.x + bounds.max.x), 0.5f * (bounds.min.z + bounds.max.z));
    for (const int cell : cells) {
        const int column = cell % grid.columns;
        const int row = cell / grid.columns;
        const glm::vec2 centre(-0.5f * span.x + (static_cast<float>(column) + 0.5f) * grid.cell.x,
                               0.5f * span.y - (static_cast<float>(row) + 0.5f) * grid.cell.y);
        // The turn takes the pad's (x, y, z) to (x, -z, y): its back edge on top, its top facing out.
        const glm::mat4 at =
            translation(glm::vec3(centre.x - padCentre.x, centre.y + padCentre.y, boardFront + kPadOff)) * faceOut;
        for (const PrefabPart* part : pad.parts) {
            out.part(part->materialRole).append(part->mesh, at);
        }
        const float hookY = centre.y + 0.5f * (bounds.max.z - bounds.min.z) + 0.004f;
        const float hook = std::min(kHookLength, p.depth - boardFront - 0.002f);
        for (const float side : {-0.3f, 0.3f}) {
            const float x = centre.x + side * (bounds.max.x - bounds.min.x);
            metal.cylinder(0.0025f, hook, 8, true,
                           glm::rotate(translation(glm::vec3(x, hookY, boardFront + 0.5f * hook)), 0.5f * core::kPi,
                                       glm::vec3(1.0f, 0.0f, 0.0f)));
        }
    }
    return std::move(out).finish();
}

// --- coat_stand -----------------------------------------------------------------------------------------------------

constexpr std::string_view kCoatStand = "coat_stand";
constexpr float kPoleRadius = 0.022f;
constexpr float kFootHeight = 0.035f;
constexpr int kFeet = 4;

Status validate(const CoatStandParams& p) {
    return firstError({checkRange(kCoatStand, "height", p.height, 1.2f, 2.2f),
                       checkRange(kCoatStand, "spread", p.spread, 0.18f, 0.3f),
                       checkRange(kCoatStand, "seed", p.seed, 0, kMaxSeed)});
}

/// The horizontal unit at `angle` radians from +Z towards +X.
[[nodiscard]] glm::vec3 around(float angle) {
    return {std::sin(angle), 0.0f, std::cos(angle)};
}

ProceduralPrefab assemble(const CoatStandParams& p) {
    detail::PrefabAssembler out(kCoatStand, {"wood", "metal", "fabric", "fabric_alt"});
    core::Random random(static_cast<std::uint64_t>(p.seed) * 8u + 3u);
    MeshBuilder& wood = out.part("wood");
    // Four feet along the axes, each from the pole out to the spread: the bounds are exactly the spread every way, so
    // that the stand stands where a socket's piece stands to the millimetre.
    const float footWidth = 0.04f;
    for (int i = 0; i < kFeet; ++i) {
        const float angle = 0.5f * core::kPi * static_cast<float>(i);
        const glm::mat4 frame =
            glm::rotate(translation(around(angle) * (0.5f * p.spread) + glm::vec3(0.0f, 0.5f * kFootHeight, 0.0f)),
                        angle, glm::vec3(0.0f, 1.0f, 0.0f));
        wood.box(glm::vec3(footWidth, kFootHeight, p.spread), frame, kGrain, kWoodTile);
    }
    // The pole, its collar over the feet, the knob on top.
    const float poleTop = p.height - 0.05f;
    wood.cylinder(0.04f, 0.06f, 20, true, translation(glm::vec3(0.0f, kFootHeight + 0.03f, 0.0f)));
    wood.cylinder(kPoleRadius, poleTop, 20, true, translation(glm::vec3(0.0f, 0.5f * poleTop, 0.0f)));
    wood.uvSphere(0.032f, 16, 10, translation(glm::vec3(0.0f, p.height - 0.032f, 0.0f)));
    // Four hooks on the diagonals rising outwards, a ball at each end.
    MeshBuilder& metal = out.part("metal");
    const float hookY = p.height - 0.16f;
    const float hookReach = std::min(0.13f, p.spread - 0.03f);
    std::array<glm::vec3, 4> tips{};
    for (int i = 0; i < 4; ++i) {
        const float angle = 0.25f * core::kPi + 0.5f * core::kPi * static_cast<float>(i);
        const glm::vec3 dir = glm::normalize(around(angle) + glm::vec3(0.0f, 0.35f, 0.0f));
        const float length = hookReach / glm::length(glm::vec3(dir.x, 0.0f, dir.z));
        const glm::vec3 start(0.0f, hookY, 0.0f);
        const glm::vec3 tip = start + dir * length;
        tips[static_cast<std::size_t>(i)] = tip;
        const glm::vec3 axis = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), dir));
        const float tilt = std::acos(std::clamp(dir.y, -1.0f, 1.0f));
        metal.cylinder(0.005f, length, 8, true, glm::rotate(translation(0.5f * (start + tip)), tilt, axis));
        metal.uvSphere(0.009f, 10, 6, translation(tip));
    }
    // What hangs, by the seed: a bag on its strap, a scarf, a cap (each on its own hook, one or two hooks bare).
    std::array<int, 4> hooks{0, 1, 2, 3};
    for (std::size_t i = hooks.size(); i > 1; --i) {
        std::swap(hooks[i - 1], hooks[static_cast<std::size_t>(random.below(i))]);
    }
    const auto hookAngle = [](int hook) {
        return 0.25f * core::kPi + 0.5f * core::kPi * static_cast<float>(hook);
    };
    {
        // The bag: hanging under its hook's tip, its broad side across the hook, its strap up to the tip.
        const int hook = hooks[0];
        const glm::vec3 tip = tips[static_cast<std::size_t>(hook)];
        const float angle = hookAngle(hook);
        const glm::vec3 bag(0.2f, 0.26f, 0.06f);
        const glm::vec3 radial = around(angle);
        const glm::vec3 centre(radial.x * (glm::length(glm::vec2(tip.x, tip.z)) - 0.01f), tip.y - 0.22f - 0.5f * bag.y,
                               radial.z * (glm::length(glm::vec2(tip.x, tip.z)) - 0.01f));
        const glm::mat4 frame = glm::rotate(translation(centre), angle, glm::vec3(0.0f, 1.0f, 0.0f));
        out.part("fabric").chamferedBox(glm::vec3(bag.x, bag.y, bag.z), 0.012f, frame);
        out.part("fabric").box(
            glm::vec3(0.02f, 0.22f, 0.006f),
            glm::rotate(translation(glm::vec3(centre.x, tip.y - 0.11f, centre.z)), angle, glm::vec3(0.0f, 1.0f, 0.0f)));
    }
    {
        // The scarf: two long ends hanging from its hook, one a little longer.
        const int hook = hooks[1];
        const glm::vec3 tip = tips[static_cast<std::size_t>(hook)];
        const float angle = hookAngle(hook);
        const float r = glm::length(glm::vec2(tip.x, tip.z)) - 0.012f;
        const glm::vec3 radial = around(angle);
        const glm::vec3 across = glm::vec3(radial.z, 0.0f, -radial.x);
        const float lengths[2] = {uniform(random, 0.5f, 0.6f), uniform(random, 0.6f, 0.72f)};
        for (int end = 0; end < 2; ++end) {
            const float length = lengths[end];
            const glm::vec3 centre =
                radial * r + across * (end == 0 ? -0.035f : 0.035f) + glm::vec3(0.0f, tip.y - 0.5f * length, 0.0f);
            out.part("fabric_alt")
                .box(glm::vec3(0.07f, length, 0.012f),
                     glm::rotate(translation(centre), angle, glm::vec3(0.0f, 1.0f, 0.0f)));
        }
    }
    if (random.below(3) != 0) {
        // The cap on the third hook: a crown and a brim pointing out.
        const int hook = hooks[2];
        const glm::vec3 tip = tips[static_cast<std::size_t>(hook)];
        const float angle = hookAngle(hook);
        const glm::vec3 radial = around(angle);
        const float r = std::min(glm::length(glm::vec2(tip.x, tip.z)), p.spread - 0.1f);
        const glm::vec3 crown = radial * r + glm::vec3(0.0f, tip.y - 0.02f, 0.0f);
        out.part("fabric_alt").uvSphere(0.075f, 16, 8, glm::scale(translation(crown), glm::vec3(1.0f, 0.7f, 1.0f)));
        out.part("fabric_alt")
            .box(glm::vec3(0.12f, 0.008f, 0.07f),
                 glm::rotate(translation(crown + radial * 0.075f + glm::vec3(0.0f, -0.035f, 0.0f)), angle,
                             glm::vec3(0.0f, 1.0f, 0.0f)));
    }
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildBookcase(const BookcaseParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

Expected<ProceduralPrefab> buildRecordRack(const RecordRackParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

Expected<ProceduralPrefab> buildPegboard(const PegboardParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

Expected<ProceduralPrefab> buildCoatStand(const CoatStandParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
