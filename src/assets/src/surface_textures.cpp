#include <rv/assets/surface_textures.hpp>

#include <algorithm>
#include <array>
#include <string>

#include "surface_canvas.hpp"

namespace rv::assets {
namespace {

using Generator = ProceduralSurface (*)(const ProceduralSurfaceInfo&);

struct CatalogEntry {
    ProceduralSurfaceInfo info;
    Generator generate;
};

const std::array<CatalogEntry, 15>& catalog() {
    static const std::array<CatalogEntry, 15> entries{{
        {{"oak_planks", {1024, 1024}, {2.0f, 2.0f}, true, true,
          "Oak floor planks 14.3 cm wide running along v, staggered joints with a dark gap and a micro-bevel, a tint "
          "and a slight cup per plank, satin varnish with worn traffic areas. Full surface (white base color, "
          "roughness factor 1); one repeat covers 2 x 2 m: uv = meters / 2 (room_shell metersPerTile 2)."},
         &detail::makeOakPlanks},
        {{"walnut", {1024, 1024}, {1.0f, 1.0f}, true, true,
          "Walnut boards: dark chocolate grain with lighter streaks and a strong figure, pores, satin finish; the "
          "grain runs along u. Full surface; one repeat covers 1 x 1 m: uv in meters along the board (the wood of "
          "the furniture prefabs)."},
         &detail::makeWalnut},
        {{"varnished_wood", {1024, 1024}, {1.0f, 1.0f}, true, true,
          "Reddish-brown boards under a glossy varnish: straight, close grain and a quiet ribbon figure, along u. Full "
          "surface; one repeat covers 1 x 1 m: uv in meters along the board."},
         &detail::makeVarnishedWood},
        {{"oak", {512, 512}, {1.0f, 1.0f}, true, true,
          "Medium oak boards with open, ring-porous grain along u, satin finish (frames, trim). Full surface; one "
          "repeat covers 1 x 1 m: uv in meters along the board."},
         &detail::makeOak},
        {{"painted_plaster", {512, 512}, {2.0f, 2.0f}, true, true,
          "Smoky grey-brown matte paint over troweled plaster: soft clouds of tone and sheen, fine sand, a gentle "
          "undulation. Full surface; one repeat covers 2 x 2 m: uv = meters / 2 (room_shell metersPerTile 2)."},
         &detail::makePaintedPlaster},
        {{"ornate_rug", {1536, 1024}, {2.4f, 1.6f}, false, false,
          "Ornamental medallion rug in the style palette (bordeaux field, navy, cream, muted gold): borders, a lattice "
          "field, a central medallion; wool fibers, dye bands and wear. A picture over uv 0..1 of a rug's top face, "
          "designed for 2.4 x 1.6 m (the rug prefab's default size); its outer guard band is uniform, so its edges "
          "join seamlessly when repeated. Full surface, no normal map."},
         &detail::makeOrnateRug},
        // I6 step 3b: the rugs the packs choose (ADR-0014 §1 amended), a little coarser than the classic one.
        {{"ornate_rug_blue", {1152, 768}, {2.4f, 1.6f}, false, false,
          "The ornamental rug's design in the optical era's colours: a deep blue field, lattice and borders, red "
          "motifs, cream, muted gold and a cold blue (concept panel 4). A picture like ornate_rug's; no normal map."},
         &detail::makeOrnateRugBlue},
        {{"ornate_rug_sage", {1152, 768}, {2.4f, 1.6f}, false, false,
          "The ornamental rug's design muted for the handheld archive: a sage field, slate-green borders, terracotta, "
          "cream and a soft teal. A picture like ornate_rug's; no normal map."},
         &detail::makeOrnateRugSage},
        {{"ornate_rug_green", {1152, 768}, {2.4f, 1.6f}, false, false,
          "The ornamental rug's design for the computer studio: a bottle-green field, near-black green borders, olive "
          "motifs and an old gold. A picture like ornate_rug's; no normal map."},
         &detail::makeOrnateRugGreen},
        {{"geometric_rug", {1152, 768}, {2.4f, 1.6f}, false, false,
          "The 90s collector's rug: confetti of the decade (triangles, rings, zigzags, dots, squares and bars in teal, "
          "magenta, yellow, violet and cream, one per 20 cm cell) on charcoal inside a teal border and a yellow line. "
          "A picture over uv 0..1 of a 2.4 x 1.6 m rug; no normal map."},
         &detail::makeGeometricRug},
        {{"runner_rug", {512, 1280}, {1.0f, 2.5f}, false, false,
          "The arcade's runner (concept panel 5): the ornamental rug's borders around a field of lozenge medallions "
          "repeated along its length, red on a near-black ground. A picture over uv 0..1 of a 1.0 x 2.5 m runner "
          "(u across it, v along it); no normal map."},
         &detail::makeRunnerRug},
        {{"brushed_metal", {512, 512}, {0.25f, 0.25f}, true, true,
          "Satin steel brushed along u: streaks of brightness and sheen, fine scratches across them; metallic. Full "
          "surface (white base color, roughness and metallic factors 1); one repeat covers 25 x 25 cm."},
         &detail::makeBrushedMetal},
        {{"aged_plastic", {512, 512}, {0.5f, 0.5f}, true, true,
          "Detail surface for plastics: base color and roughness close to 1, which multiply the material's own color "
          "and roughness (blotches of yellowing, speckle, smoother handled patches), and an orange-peel micro-relief. "
          "One repeat per uv unit (the faces of the plastic prefabs)."},
         &detail::makeAgedPlastic},
        {{"wallwash", {128, 192}, {1.0f, 1.5f}, false, false,
          "The soft light a wall sconce throws on its wall (I6 step 2): a white picture whose alpha is a strong scallop "
          "rising from the shade, a weaker one below it and a soft core, zero at the edges. A picture over uv 0..1 of "
          "a 1.0 x 1.5 m quad centred 5 cm above the shade, for an unlit, alpha-blended material (its colour and "
          "strength are the material's); no normal map."},
         &detail::makeWallWash},
        {{"shelfglow", {128, 64}, {0.8f, 0.4f}, false, false,
          "The light a shelf's strip throws down the back panel of its compartment (I6 step 3): a white picture whose "
          "alpha falls from the top edge (under the strip) towards the board below, fading at the sides. A picture over "
          "uv 0..1 of a compartment's back panel, for an unlit, alpha-blended material (its colour and strength are the "
          "material's); no normal map."},
         &detail::makeShelfGlow},
    }};
    return entries;
}

const CatalogEntry* findEntry(std::string_view name) noexcept {
    const auto& entries = catalog();
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [name](const CatalogEntry& entry) { return entry.info.name == name; });
    return found != entries.end() ? &*found : nullptr;
}

std::string surfaceNameList() {
    std::string names;
    for (const CatalogEntry& entry : catalog()) {
        names += names.empty() ? "" : ", ";
        names += entry.info.name;
    }
    return names;
}

} // namespace

std::string_view toString(SurfaceMap map) noexcept {
    switch (map) {
    case SurfaceMap::BaseColor:
        return "baseColor";
    case SurfaceMap::MetallicRoughness:
        return "metallicRoughness";
    case SurfaceMap::Normal:
        return "normal";
    }
    return "baseColor";
}

std::optional<SurfaceMap> surfaceMapFromString(std::string_view name) noexcept {
    for (const SurfaceMap map : {SurfaceMap::BaseColor, SurfaceMap::MetallicRoughness, SurfaceMap::Normal}) {
        if (toString(map) == name) {
            return map;
        }
    }
    return std::nullopt;
}

std::span<const ProceduralSurfaceInfo> proceduralSurfaces() noexcept {
    static const std::array<ProceduralSurfaceInfo, 15> infos = [] {
        std::array<ProceduralSurfaceInfo, 15> list{};
        std::transform(catalog().begin(), catalog().end(), list.begin(),
                       [](const CatalogEntry& entry) { return entry.info; });
        return list;
    }();
    return infos;
}

const ProceduralSurfaceInfo* findProceduralSurface(std::string_view name) noexcept {
    const std::span<const ProceduralSurfaceInfo> infos = proceduralSurfaces();
    const auto found = std::find_if(infos.begin(), infos.end(),
                                    [name](const ProceduralSurfaceInfo& info) { return info.name == name; });
    return found != infos.end() ? &*found : nullptr;
}

const ImageData& ProceduralSurface::map(SurfaceMap which) const noexcept {
    switch (which) {
    case SurfaceMap::BaseColor:
        return baseColor;
    case SurfaceMap::MetallicRoughness:
        return metallicRoughness;
    case SurfaceMap::Normal:
        return normal;
    }
    return baseColor;
}

Expected<ProceduralSurface> makeProceduralSurface(std::string_view name) {
    const CatalogEntry* entry = findEntry(name);
    if (entry == nullptr) {
        return makeError(ErrorCode::NotFound, "unknown procedural surface (known: " + surfaceNameList() + ")",
                         std::string(name));
    }
    return entry->generate(entry->info);
}

std::string ProceduralTextureName::toString() const {
    return surface + "/" + std::string(assets::toString(map));
}

Expected<ProceduralTextureName> parseProceduralTextureName(std::string_view text) {
    const std::size_t slash = text.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 == text.size() ||
        text.find('/', slash + 1) != std::string_view::npos) {
        return makeError(ErrorCode::InvalidArgument,
                         "procedural textures are named \"<surface>/<map>\", e.g. \"oak_planks/baseColor\"",
                         std::string(text));
    }
    const std::string_view surface = text.substr(0, slash);
    const std::string_view mapName = text.substr(slash + 1);
    const ProceduralSurfaceInfo* info = findProceduralSurface(surface);
    if (info == nullptr) {
        return makeError(ErrorCode::NotFound, "unknown procedural surface (known: " + surfaceNameList() + ")",
                         std::string(text));
    }
    const std::optional<SurfaceMap> map = surfaceMapFromString(mapName);
    if (!map) {
        return makeError(ErrorCode::NotFound, "unknown map (expected baseColor, metallicRoughness or normal)",
                         std::string(text));
    }
    if (*map == SurfaceMap::Normal && !info->hasNormalMap) {
        return makeError(ErrorCode::NotFound, "this surface has no normal map", std::string(text));
    }
    return ProceduralTextureName{std::string(surface), *map};
}

} // namespace rv::assets
