#include <rv/world/media_family.hpp>

#include <cmath>
#include <set>

#include <rv/core/utf8.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>

#include "data_reader.hpp"

namespace rv::world {
namespace {

using detail::DocumentReader;
using detail::ObjectReader;
using Json = nlohmann::json;

/// Items from 1 mm to 2 m; gaps, margins and headroom up to 1 m.
constexpr std::int64_t kMaxItemMm = 2'000;
constexpr std::int64_t kMaxClearanceMm = 1'000;

SlotSpec readSlot(const ObjectReader& reader) {
    SlotSpec slot;
    slot.widthMm = reader.integer("widthMm", 1, kMaxItemMm);
    slot.heightMm = reader.integer("heightMm", 1, kMaxItemMm);
    slot.depthMm = reader.integer("depthMm", 1, kMaxItemMm);
    slot.gapMm = reader.integer("gapMm", 0, kMaxClearanceMm);
    slot.marginMm = reader.integer("marginMm", 0, kMaxClearanceMm);
    slot.headroomMm = reader.integer("headroomMm", 0, kMaxClearanceMm);
    return slot;
}

void requireSize(DocumentReader& document, const std::string& pointer, const SlotSpec& slot, std::int64_t width,
                 std::int64_t height, std::int64_t depth, std::string_view orientation) {
    if (!document.failed() && (slot.widthMm != width || slot.heightMm != height || slot.depthMm != depth)) {
        document.fail(pointer, "the " + std::string(orientation) + " slot must be " + std::to_string(width) + " x " +
                                   std::to_string(height) + " x " + std::to_string(depth) +
                                   " mm, the item in that orientation");
    }
}

/// A fixture of the family: a prefab of `kind`, or nothing when the key is not there.
std::optional<PrefabRef> readFixture(const ObjectReader& reader, std::string_view key, std::string_view kind) {
    if (!reader.has(key)) {
        return std::nullopt;
    }
    PrefabRef ref = reader.prefab(key);
    if (!reader.document().failed() && ref.kind != kind) {
        reader.document().fail(reader.pointer(key),
                               "the fixture must be a '" + std::string(kind) + "', not a '" + ref.kind + "'");
    }
    return ref;
}

MediaFamily readFamily(const ObjectReader& reader) {
    MediaFamily family;
    family.id = reader.dataId("id");
    family.item = reader.prefab("item");
    family.itemWidthMm = reader.integer("itemWidthMm", 1, kMaxItemMm);
    family.itemHeightMm = reader.integer("itemHeightMm", 1, kMaxItemMm);
    family.itemDepthMm = reader.integer("itemDepthMm", 1, kMaxItemMm);
    family.labelWidthMm = reader.integer("labelWidthMm", 1, kMaxItemMm);
    family.labelHeightMm = reader.integer("labelHeightMm", 1, kMaxItemMm);
    if (!reader.document().failed() &&
        (family.labelWidthMm > family.itemWidthMm || family.labelHeightMm > family.itemHeightMm)) {
        reader.document().fail(reader.pointer("labelWidthMm"), "the label is larger than the item's front");
    }
    family.coverFit = reader.enumeration<CoverFit>("coverFit", parseCoverFit, "one of label, layer");
    family.spineOut =
        readSlot(reader.object("spineOut", {"depthMm", "gapMm", "headroomMm", "heightMm", "marginMm", "widthMm"}));
    family.faceOut =
        readSlot(reader.object("faceOut", {"depthMm", "gapMm", "headroomMm", "heightMm", "marginMm", "widthMm"}));
    if (reader.has("spineFacing")) {
        family.spineFacing = reader.enumeration<SpineFacing>("spineFacing", parseSpineFacing, "one of end, side");
    }
    if (reader.has("succeeds")) {
        family.succeeds = reader.dataId("succeeds");
    }
    // Stood on its end with its top face outward, the spine is the item's depth wide and its width tall; stood on its
    // side, upright, the item's depth wide and its height tall (I6 step 4).
    if (family.spineFacing == SpineFacing::Side) {
        requireSize(reader.document(), reader.pointer("spineOut"), family.spineOut, family.itemDepthMm,
                    family.itemHeightMm, family.itemWidthMm, "spine-out (on its side)");
    } else {
        requireSize(reader.document(), reader.pointer("spineOut"), family.spineOut, family.itemDepthMm,
                    family.itemWidthMm, family.itemHeightMm, "spine-out");
    }
    requireSize(reader.document(), reader.pointer("faceOut"), family.faceOut, family.itemWidthMm, family.itemHeightMm,
                family.itemDepthMm, "face-out");
    // A level's facing is told by its slot: the two must differ.
    if (!reader.document().failed() && family.spineOut.widthMm == family.faceOut.widthMm &&
        family.spineOut.heightMm == family.faceOut.heightMm && family.spineOut.depthMm == family.faceOut.depthMm) {
        reader.document().fail(reader.pointer("spineOut"), "the spine-out slot is the face-out one");
    }
    if (reader.has("fixtures")) {
        const ObjectReader fixtures = reader.object("fixtures", {"display", "heroModule", "sideModule", "wallShelf"});
        family.fixtures.heroModule = readFixture(fixtures, "heroModule", "shelf");
        family.fixtures.sideModule = readFixture(fixtures, "sideModule", "shelf");
        family.fixtures.display = readFixture(fixtures, "display", "display_case");
        family.fixtures.wallShelf = readFixture(fixtures, "wallShelf", "wall_shelf");
    }
    return family;
}

std::int64_t millimetres(float metres) {
    return static_cast<std::int64_t>(std::lround(static_cast<double>(metres) * 1000.0));
}

} // namespace

assets::LabelAspect MediaFamily::coverAspect() const noexcept {
    if (coverFit == CoverFit::Layer || labelWidthMm <= 0 || labelHeightMm <= 0) {
        return {};
    }
    return {static_cast<std::uint32_t>(labelWidthMm), static_cast<std::uint32_t>(labelHeightMm)};
}

const MediaFamily* MediaFamilyRegistry::find(std::string_view id) const noexcept {
    for (const MediaFamily& family : families) {
        if (family.id == id) {
            return &family;
        }
    }
    return nullptr;
}

const MediaFamily* MediaFamilyRegistry::successorOf(std::string_view id) const noexcept {
    for (const MediaFamily& family : families) {
        if (!family.succeeds.empty() && family.succeeds == id) {
            return &family;
        }
    }
    return nullptr;
}

const MediaFamily* MediaFamilyRegistry::latestSuccessor(std::string_view id) const noexcept {
    const MediaFamily* family = find(id);
    // The reader keeps the chains free of cycles: each family succeeded by one at most, none by itself; the count
    // bounds the walk all the same.
    for (std::size_t step = 0; family != nullptr && step < families.size(); ++step) {
        const MediaFamily* next = successorOf(family->id);
        if (next == nullptr) {
            break;
        }
        family = next;
    }
    return family;
}

Expected<MediaFamilyRegistry> parseMediaFamilies(std::string_view text, std::string_view context) {
    auto value = detail::parseDataJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    DocumentReader document(text, std::string(context));
    const ObjectReader reader(document, *value, "", {"families", "schemaVersion"});
    (void)reader.integer("schemaVersion", 1, 1);
    MediaFamilyRegistry registry;
    std::set<std::string> ids;
    detail::forEachElement(reader.array("families"), "/families", [&](const Json& element, const std::string& pointer) {
        MediaFamily family = readFamily(
            ObjectReader(document, element, pointer,
                         {"coverFit", "faceOut", "fixtures", "id", "item", "itemDepthMm", "itemHeightMm", "itemWidthMm",
                          "labelHeightMm", "labelWidthMm", "spineFacing", "spineOut", "succeeds"}));
        if (!document.failed() && !ids.insert(family.id).second) {
            document.fail(detail::pointerChild(pointer, "id"), "the family appears twice");
        }
        registry.families.push_back(std::move(family));
    });
    if (!document.failed() && registry.families.empty()) {
        document.fail("/families", "must name at least one family");
    }
    // Successors (I6 step 4): each names another family of the file, and a family has one successor at most, so that
    // every chain ends.
    std::set<std::string> succeeded;
    for (std::size_t index = 0; !document.failed() && index < registry.families.size(); ++index) {
        const MediaFamily& family = registry.families[index];
        if (family.succeeds.empty()) {
            continue;
        }
        const std::string pointer = "/families/" + std::to_string(index) + "/succeeds";
        if (family.succeeds == family.id) {
            document.fail(pointer, "a family cannot succeed itself");
        } else if (registry.find(family.succeeds) == nullptr) {
            document.fail(pointer, "no family " + family.succeeds + " to succeed");
        } else if (!succeeded.insert(family.succeeds).second) {
            document.fail(pointer, "the family " + family.succeeds + " has a successor already");
        }
    }
    if (document.failed()) {
        return propagate(document.error());
    }
    return registry;
}

Expected<MediaFamilyRegistry> loadMediaFamilies(const std::filesystem::path& assetRoot) {
    const std::filesystem::path path = assetRoot / "world" / "media_families.json";
    auto text = detail::readDataFile(path);
    if (!text) {
        return propagate(text.error());
    }
    return parseMediaFamilies(*text, core::pathToUtf8Lossy(path));
}

Status checkItemManifest(const MediaFamily& family, const assets::PrefabManifest& manifest) {
    const std::string context = family.id + ".item";
    if (manifest.id != family.item.manifest) {
        return makeError(ErrorCode::ValidationFailed,
                         "the manifest is \"" + manifest.id + "\", the family names \"" + family.item.manifest + "\"",
                         context);
    }
    if (manifest.runtime.generator.value_or("") != family.item.kind) {
        return makeError(ErrorCode::ValidationFailed,
                         "the manifest builds \"" + manifest.runtime.generator.value_or("") +
                             "\", the family names \"" + family.item.kind + "\"",
                         context);
    }
    if (!manifest.runtime.scaleMeters) {
        return makeError(ErrorCode::ValidationFailed, "the manifest gives no runtime.scaleMeters", context);
    }
    const glm::vec3 meters = *manifest.runtime.scaleMeters;
    const std::int64_t width = millimetres(meters.x);
    const std::int64_t height = millimetres(meters.y);
    const std::int64_t depth = millimetres(meters.z);
    if (width != family.itemWidthMm || height != family.itemHeightMm || depth != family.itemDepthMm) {
        return makeError(ErrorCode::ValidationFailed,
                         "the manifest's scaleMeters is " + std::to_string(width) + " x " + std::to_string(height) +
                             " x " + std::to_string(depth) + " mm, the family's item " +
                             std::to_string(family.itemWidthMm) + " x " + std::to_string(family.itemHeightMm) + " x " +
                             std::to_string(family.itemDepthMm) + " mm",
                         context);
    }
    // The prefab the manifest builds: as large as the item, its label quad the family's (what covers are fitted to).
    auto prefab = geometry::buildPrefab(family.item.kind, manifest.runtime.params.is_null() ? Json::object()
                                                                                           : manifest.runtime.params);
    if (!prefab) {
        Error error = prefab.error();
        error.message = "the manifest's prefab does not build: " + error.message;
        error.code = ErrorCode::ValidationFailed;
        error.context = context;
        return propagate(std::move(error));
    }
    const glm::vec3 built = prefab->bounds.max - prefab->bounds.min;
    if (millimetres(built.x) != family.itemWidthMm || millimetres(built.y) != family.itemHeightMm ||
        millimetres(built.z) != family.itemDepthMm) {
        return makeError(ErrorCode::ValidationFailed,
                         "the manifest's prefab is " + std::to_string(millimetres(built.x)) + " x " +
                             std::to_string(millimetres(built.y)) + " x " + std::to_string(millimetres(built.z)) +
                             " mm, not the item's size",
                         context);
    }
    const geometry::Socket* label = geometry::findSocket(prefab->sockets, "Label");
    if (label == nullptr) {
        return makeError(ErrorCode::ValidationFailed, "the manifest's prefab has no Label socket", context);
    }
    const glm::vec3 labelSize = label->bounds.max - label->bounds.min;
    if (millimetres(labelSize.x) != family.labelWidthMm || millimetres(labelSize.y) != family.labelHeightMm) {
        return makeError(ErrorCode::ValidationFailed,
                         "the manifest's label is " + std::to_string(millimetres(labelSize.x)) + " x " +
                             std::to_string(millimetres(labelSize.y)) + " mm, the family's " +
                             std::to_string(family.labelWidthMm) + " x " + std::to_string(family.labelHeightMm) +
                             " mm",
                         context);
    }
    // The spine label lies on the face the family's items show spine-out (I6 step 4): out of the top face standing on
    // their end, out of the -X face standing on their side.
    if (const geometry::Socket* spine = geometry::findSocket(prefab->sockets, "Spine")) {
        const glm::vec3 out(spine->transform[2]);
        const glm::vec3 expected =
            family.spineFacing == SpineFacing::Side ? glm::vec3(-1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        if (glm::dot(out, expected) < 0.999f) {
            return makeError(ErrorCode::ValidationFailed,
                             "the manifest's spine label does not face out of the " +
                                 std::string(family.spineFacing == SpineFacing::Side ? "-X face" : "top face") +
                                 " (the family's spineFacing " + std::string(toString(family.spineFacing)) + ")",
                             context);
        }
    }
    return ok();
}

RoomTemplate withFamilyFixtures(RoomTemplate roomTemplate, const MediaFamily& family) {
    const FamilyFixtures& fixtures = family.fixtures;
    if (fixtures.heroModule) {
        roomTemplate.heroModule = *fixtures.heroModule;
    }
    if (fixtures.sideModule) {
        roomTemplate.sideModule = *fixtures.sideModule;
    }
    for (TemplateSocket& socket : roomTemplate.sockets) {
        if (socket.kind != SocketKind::Display || !socket.prefab) {
            continue;
        }
        if (socket.prefab->kind == "display_case" && fixtures.display) {
            socket.prefab = *fixtures.display;
        } else if (socket.prefab->kind == "wall_shelf" && fixtures.wallShelf) {
            socket.prefab = *fixtures.wallShelf;
        }
    }
    return roomTemplate;
}

} // namespace rv::world
