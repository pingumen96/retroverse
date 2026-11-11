#include <rv/app/shelf_labels.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <tuple>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <rv/catalog/metadata.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/world/floor_layout.hpp>
#include <rv/world/room_look.hpp>

namespace rv::app {

namespace {

constexpr std::string_view kEnDash = "–";
constexpr std::string_view kMiddleDot = " · ";
/// A section sign's margin to its module's sides, its rise over its top and its set back from its front.
constexpr float kSignSideMargin = 0.03F;
constexpr float kSignRise = 0.002F;
constexpr float kSignSetBack = 0.002F;
/// The narrowest section sign.
constexpr float kMinSignWidth = 0.12F;

glm::mat4 translated(const glm::mat4& frame, float x, float y, float z) {
    return glm::translate(frame, glm::vec3(x, y, z));
}

nlohmann::json frameJson(const glm::mat4& frame) {
    nlohmann::json values = nlohmann::json::array();
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 3; ++row) {
            values.push_back(frame[column][row]);
        }
    }
    return values;
}

/// The fixtures a section sign stands on: the modules, the wall shelf, the display cases.
bool holdsSectionSign(std::string_view generator) noexcept {
    return generator == "shelf" || generator == "wall_shelf" || generator == "display_case";
}

const world::ScenePrefab* builtPrefab(const world::SceneData& data, const world::Fixture& fixture) {
    const world::ScenePrefab* prefab = data.prefabs.find(fixture.prefab);
    return prefab != nullptr && prefab->prefab ? prefab : nullptr;
}

std::string slotSocketId(std::int64_t level, std::int64_t index) {
    return "Slot_" + std::to_string(level) + "_" + std::to_string(index);
}

/// The pitch of slot `index` of `level` of `prefab`: the distance to its nearest neighbour across, else its width.
float slotPitch(const geometry::ProceduralPrefab& prefab, const geometry::Socket& slot, std::uint32_t level,
                std::uint32_t index) {
    std::optional<float> nearest;
    for (const std::int64_t neighbour : {static_cast<std::int64_t>(index) - 1, static_cast<std::int64_t>(index) + 1}) {
        if (neighbour < 0) {
            continue;
        }
        if (const geometry::Socket* other = geometry::findSocket(prefab.sockets, slotSocketId(level, neighbour))) {
            const float distance = std::abs(other->transform[3].x - slot.transform[3].x);
            nearest = nearest ? std::min(*nearest, distance) : distance;
        }
    }
    return nearest.value_or(slot.bounds.max.x - slot.bounds.min.x);
}

scene::TextureSource pattern(std::string_view name, std::string title) {
    scene::TextureSource picture;
    picture.kind = scene::TextureSource::Kind::Pattern;
    picture.pattern = std::string(name);
    picture.title = std::move(title);
    return picture;
}

scene::SceneObject plateObject(std::string id, const std::vector<geometry::LabelPlate>& plates, float inset,
                               std::map<std::string, std::string> materials, std::string title) {
    nlohmann::json list = nlohmann::json::array();
    for (const geometry::LabelPlate& plate : plates) {
        list.push_back(nlohmann::json{{"frame", frameJson(plate.frame)},
                                      {"size", {plate.size.x, plate.size.y}},
                                      {"cell", {plate.cell.x, plate.cell.y, plate.cell.z, plate.cell.w}}});
    }
    scene::SceneObject object;
    object.id = std::move(id);
    object.source = scene::SceneObject::Source::Prefab;
    object.kind = "label_plates";
    object.params = nlohmann::json{{"plates", std::move(list)}, {"thickness", kPlateThickness}, {"inset", inset}};
    object.materialsByRole = std::move(materials);
    object.texturesByRole.emplace("picture", pattern("cover", std::move(title)));
    object.castsShadows = false;
    object.collides = false;
    return object;
}

} // namespace

std::string shelfInitial(std::string_view sortKey) {
    if (sortKey.empty()) {
        return "#";
    }
    const char c = sortKey.front();
    if (c >= 'a' && c <= 'z') {
        return std::string(1, static_cast<char>(c - 'a' + 'A'));
    }
    if (c >= 'A' && c <= 'Z') {
        return std::string(1, c);
    }
    if (c >= '0' && c <= '9') {
        return "0-9";
    }
    return "#";
}

int initialRank(std::string_view initial) noexcept {
    if (initial == "#") {
        return 0;
    }
    if (initial == "0-9") {
        return 1;
    }
    if (initial.size() == 1 && initial.front() >= 'A' && initial.front() <= 'Z') {
        return 2 + (initial.front() - 'A');
    }
    return 100;
}

std::string letterRun(std::string_view first, std::string_view last) {
    if (first == last) {
        return std::string(first);
    }
    std::string run(first == "0-9" ? std::string_view("0") : first);
    run += kEnDash;
    run += last == "0-9" ? std::string_view("9") : last;
    return run;
}

std::string captionDetail(std::string_view year, std::string_view developer, std::string_view publisher) {
    const std::string_view maker = developer.empty() ? publisher : developer;
    if (year.empty()) {
        return std::string(maker);
    }
    if (maker.empty()) {
        return std::string(year);
    }
    std::string detail(year);
    detail += kMiddleDot;
    detail += maker;
    return detail;
}

ShelfText shelfTextOf(const RoomItemText& text, std::optional<std::size_t> section) {
    ShelfText shelf;
    shelf.game = text.game;
    shelf.title = text.title.empty() ? text.fallbackTitle : text.title;
    shelf.detail = captionDetail(text.year, text.developer, text.publisher);
    if (text.screen && !text.screen->sortKey.empty()) {
        shelf.sortKey = text.screen->sortKey;
    } else if (auto key = catalog::sortKeyOf(shelf.title)) {
        shelf.sortKey = std::move(*key);
    }
    shelf.initial = shelfInitial(shelf.sortKey);
    shelf.section = section;
    return shelf;
}

const ShelfText* RoomShelfTexts::find(const catalog::GameId& game) const noexcept {
    const auto found =
        std::lower_bound(games.begin(), games.end(), game,
                         [](const ShelfText& text, const catalog::GameId& id) { return text.game < id; });
    return found != games.end() && found->game == game ? &*found : nullptr;
}

ShelfLabels roomShelfLabels(const world::WorldPlan& plan, const world::RoomId& room, const world::SceneData& data,
                            const world::CollectionLayout& layout, const world::RoomPlacement& scenePlacement,
                            const RoomShelfTexts& texts) {
    ShelfLabels labels;
    labels.room = room;
    labels.tabMaterials = data.theme.materials("label_plates");
    if (labels.tabMaterials.empty()) {
        labels.tabMaterials = world::themeMaterials("label_plates");
    }
    const auto paper = labels.tabMaterials.find("picture");
    labels.cardMaterial = paper != labels.tabMaterials.end() ? paper->second : "MAT_Paper_Print";
    const std::map<std::string, std::string> hall = data.theme.materials("hall");
    const auto hallMaterial = [&](const char* role, const char* fallback) {
        const auto found = hall.find(role);
        return found != hall.end() ? found->second : std::string(fallback);
    };
    labels.signMaterials = {{"frame", hallMaterial("trim", "MAT_Wood_Walnut")},
                            {"picture", hallMaterial("sign", "MAT_Sign_Tungsten")}};

    const auto planned = std::find_if(plan.rooms.begin(), plan.rooms.end(),
                                      [&](const world::Room& candidate) { return candidate.id == room; });
    if (planned == plan.rooms.end()) {
        return labels;
    }
    std::map<world::FixtureId, const world::Fixture*> fixtures;
    for (const world::Fixture& fixture : plan.fixtures) {
        if (fixture.id.room() == room) {
            fixtures.emplace(fixture.id, &fixture);
        }
    }
    const glm::mat4 toScene = world::roomToFloorMatrix(scenePlacement);
    const auto fixtureFrame = [&](const world::Fixture& fixture) {
        return toScene * world::room_frames::fixtureTransform(fixture);
    };

    // The levels' initials and the cards, in the layout's order.
    struct Run {
        world::FixtureId fixture;
        std::uint32_t level = 0;
        std::string first;
        std::string last;
    };
    std::vector<Run> runs;
    for (const world::CollectionItem& item : layout.items) {
        const auto fixture = fixtures.find(item.slot.fixture());
        const ShelfText* text = texts.find(item.game);
        if (fixture == fixtures.end() || text == nullptr) {
            continue;
        }
        const world::ScenePrefab* prefab = builtPrefab(data, *fixture->second);
        if (prefab == nullptr) {
            continue;
        }
        if (prefab->generator == "shelf" && item.stance == world::ItemStance::SpineOut) {
            const auto run = std::find_if(runs.begin(), runs.end(), [&](const Run& candidate) {
                return candidate.fixture == item.slot.fixture() && candidate.level == item.slot.level();
            });
            if (run == runs.end()) {
                runs.push_back(Run{item.slot.fixture(), item.slot.level(), text->initial, text->initial});
            } else {
                if (initialRank(text->initial) < initialRank(run->first)) {
                    run->first = text->initial;
                }
                if (initialRank(text->initial) > initialRank(run->last)) {
                    run->last = text->initial;
                }
            }
        } else if (prefab->generator == "display_case" && item.stance == world::ItemStance::FaceOut &&
                   labels.captions.size() < assets::kMaxAtlasCells) {
            const geometry::Socket* slot =
                geometry::findSocket(prefab->prefab->sockets, slotSocketId(item.slot.level(), item.slot.index()));
            if (slot == nullptr) {
                continue;
            }
            const float pitch = slotPitch(*prefab->prefab, *slot, item.slot.level(), item.slot.index());
            const float width = std::clamp(pitch - kCaptionGap, 0.02F, kCaptionSize.x);
            const glm::vec2 size(width, width * kCaptionSize.y / kCaptionSize.x);
            const glm::mat4 frame = translated(fixtureFrame(*fixture->second) * slot->transform, 0.0F,
                                               geometry::kDisplayCaseLipHeight + kCaptionRise - 0.5F * size.y,
                                               slot->bounds.max.z + geometry::kDisplayCaseLipFront + kPlateGap);
            labels.captions.push_back(ShelfCaption{item.slot.fixture(), item.game,
                                                   assets::CaptionText{text->title, text->detail}, frame, size});
        }
    }
    for (const Run& run : runs) {
        if (labels.tabs.size() >= assets::kMaxAtlasCells) {
            break;
        }
        const world::Fixture& fixture = *fixtures.at(run.fixture);
        const geometry::Socket* level =
            geometry::findSocket(builtPrefab(data, fixture)->prefab->sockets, "Level_" + std::to_string(run.level));
        if (level == nullptr || level->bounds.isEmpty()) {
            continue;
        }
        const glm::mat4 frame =
            translated(fixtureFrame(fixture) * level->transform, level->bounds.min.x + kTabFromEnd + 0.5F * kTabSize.x,
                       kTabRise - 0.5F * kTabSize.y, level->bounds.max.z + kPlateGap);
        labels.tabs.push_back(ShelfTab{run.fixture, run.level, letterRun(run.first, run.last), frame});
    }

    // The section signs: each section's first item by sort key, on a module.
    if (planned->kind == world::RoomKind::Console) {
        return labels;
    }
    std::vector<std::pair<world::FixtureId, std::size_t>> firsts; // the module, the section
    for (std::size_t section = 0; section < planned->sections.size(); ++section) {
        const world::CollectionItem* first = nullptr;
        const ShelfText* firstText = nullptr;
        for (const world::CollectionItem& item : layout.items) {
            const ShelfText* text = texts.find(item.game);
            const auto fixture = fixtures.find(item.slot.fixture());
            if (text == nullptr || text->section != section || fixture == fixtures.end()) {
                continue;
            }
            const world::ScenePrefab* prefab = builtPrefab(data, *fixture->second);
            if (prefab == nullptr || !holdsSectionSign(prefab->generator)) {
                continue;
            }
            if (first == nullptr || std::tie(text->sortKey, item.game) < std::tie(firstText->sortKey, first->game)) {
                first = &item;
                firstText = text;
            }
        }
        if (first != nullptr) {
            firsts.emplace_back(first->slot.fixture(), section);
        }
    }
    for (const auto& [module, section] : firsts) {
        std::size_t count = 0;
        std::size_t place = 0;
        for (const auto& [other, otherSection] : firsts) {
            if (other == module) {
                place += otherSection < section ? 1 : 0;
                ++count;
            }
        }
        const world::Fixture& fixture = *fixtures.at(module);
        const world::ScenePrefab& prefab = *builtPrefab(data, fixture);
        const core::Aabb& bounds = prefab.prefab->bounds;
        const float usable = bounds.max.x - bounds.min.x - 2.0F * kSignSideMargin;
        const float cell = usable / static_cast<float>(count);
        const float width = std::clamp(cell - kSectionSignGap, kMinSignWidth, kSectionSignSize.x);
        const float centre = bounds.min.x + kSignSideMargin + (static_cast<float>(place) + 0.5F) * cell;
        // On a module's or the wall shelf's top at its front; on a display case's top at its back, behind the glass.
        const float back = prefab.generator == "display_case"
                               ? bounds.min.z + kSignSetBack
                               : std::max(bounds.min.z + kSignSetBack, bounds.max.z - kSectionSignDepth - kSignSetBack);
        const glm::mat4 transform =
            translated(fixtureFrame(fixture), centre, bounds.max.y + kSignRise + 0.5F * kSectionSignSize.y, back);
        const catalog::SystemName& system = planned->sections[section];
        std::string name = section < texts.sectionNames.size() && !texts.sectionNames[section].empty()
                               ? texts.sectionNames[section]
                               : system.str();
        labels.sections.push_back(
            ShelfSectionSign{section, system, std::move(name), transform, glm::vec2(width, kSectionSignSize.y)});
    }
    std::sort(labels.sections.begin(), labels.sections.end(),
              [](const ShelfSectionSign& a, const ShelfSectionSign& b) { return a.section < b.section; });
    return labels;
}

std::string labelsPrefix(std::string_view kindPrefix, std::string_view room) {
    std::string prefix(kindPrefix);
    prefix += room;
    prefix += '/';
    return prefix;
}

std::string shelfTabsId(const world::RoomId& room) {
    return labelsPrefix(kTabsPrefix, room.toString()) + "plates";
}

std::string shelfCaptionsId(const world::RoomId& room) {
    return labelsPrefix(kCaptionsPrefix, room.toString()) + "plates";
}

std::string sectionSignId(const world::RoomId& room, std::size_t section) {
    return std::string(kSectionSignPrefix) + room.toString() + "/" + std::to_string(section);
}

std::optional<std::pair<world::RoomId, std::size_t>> sectionSignOf(std::string_view objectId) {
    if (!objectId.starts_with(kSectionSignPrefix)) {
        return std::nullopt;
    }
    const std::string_view rest = objectId.substr(kSectionSignPrefix.size());
    const std::size_t slash = rest.rfind('/');
    if (slash == std::string_view::npos || slash + 1 == rest.size()) {
        return std::nullopt;
    }
    const std::string_view digits = rest.substr(slash + 1);
    std::size_t section = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), section);
    if (error != std::errc{} || end != digits.data() + digits.size() || (digits.size() > 1 && digits.front() == '0')) {
        return std::nullopt;
    }
    auto room = world::RoomId::parse(rest.substr(0, slash));
    if (!room) {
        return std::nullopt;
    }
    return std::pair{*room, section};
}

std::vector<std::string> tabTexts(const ShelfLabels& labels) {
    std::vector<std::string> texts;
    texts.reserve(labels.tabs.size());
    for (const ShelfTab& tab : labels.tabs) {
        texts.push_back(tab.text);
    }
    return texts;
}

std::vector<assets::CaptionText> captionTexts(const ShelfLabels& labels) {
    std::vector<assets::CaptionText> texts;
    texts.reserve(labels.captions.size());
    for (const ShelfCaption& caption : labels.captions) {
        texts.push_back(caption.text);
    }
    return texts;
}

std::optional<scene::SceneObject> shelfTabsObject(const ShelfLabels& labels) {
    if (labels.tabs.empty()) {
        return std::nullopt;
    }
    const assets::LabelAtlasLayout atlas =
        assets::labelAtlasLayout(labels.tabs.size(), assets::kTabCell, assets::kTabColumns);
    std::vector<geometry::LabelPlate> plates;
    plates.reserve(labels.tabs.size());
    for (std::size_t i = 0; i < labels.tabs.size(); ++i) {
        plates.push_back(geometry::LabelPlate{labels.tabs[i].frame, kTabSize, atlas.uv(i)});
    }
    return plateObject(shelfTabsId(labels.room), plates, kTabInset, labels.tabMaterials, "TABS");
}

std::optional<scene::SceneObject> shelfCaptionsObject(const ShelfLabels& labels) {
    if (labels.captions.empty()) {
        return std::nullopt;
    }
    const assets::LabelAtlasLayout atlas =
        assets::labelAtlasLayout(labels.captions.size(), assets::kCaptionCell, assets::kCaptionColumns);
    std::vector<geometry::LabelPlate> plates;
    plates.reserve(labels.captions.size());
    for (std::size_t i = 0; i < labels.captions.size(); ++i) {
        plates.push_back(geometry::LabelPlate{labels.captions[i].frame, labels.captions[i].size, atlas.uv(i)});
    }
    return plateObject(shelfCaptionsId(labels.room), plates, 0.0F,
                       {{"plate", labels.cardMaterial}, {"picture", labels.cardMaterial}}, "CAPTIONS");
}

scene::SceneObject sectionSignObject(const ShelfLabels& labels, const ShelfSectionSign& sign) {
    scene::SceneObject object;
    object.id = sectionSignId(labels.room, sign.section);
    object.source = scene::SceneObject::Source::Prefab;
    object.kind = "poster_frame";
    object.params = nlohmann::json{{"width", sign.size.x},
                                   {"height", sign.size.y},
                                   {"frameWidth", kSectionSignFrame},
                                   {"depth", kSectionSignDepth}};
    object.transform = sign.transform;
    object.materialsByRole = labels.signMaterials;
    object.texturesByRole.emplace("picture", pattern("sign", sign.name));
    object.castsShadows = false;
    object.collides = false;
    return object;
}

} // namespace rv::app
