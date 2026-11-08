#include <rv/app/identity_signs.hpp>

#include <string>
#include <utility>

namespace rv::app {

namespace {

constexpr std::string_view kHeroSign = "fx:hero.sign";
constexpr std::string_view kPortal = "/portal:";
constexpr std::string_view kFascia = "/hub/fascia-sign:";
constexpr std::string_view kHallPrefix = "hall:";

/// The hall's sign pieces under a portal: its plate and its blades.
bool isPortalPiece(std::string_view piece) noexcept {
    return piece == "sign" || piece == "blade-east" || piece == "blade-west";
}

std::optional<world::RoomId> parseRoom(std::string_view text) {
    auto room = world::RoomId::parse(text);
    if (!room) {
        return std::nullopt;
    }
    return *room;
}

} // namespace

std::optional<std::pair<world::RoomId, IdentitySignPlace>> identitySignRoom(std::string_view objectId) {
    if (objectId.starts_with(kHallPrefix)) {
        if (const std::size_t fascia = objectId.find(kFascia); fascia != std::string_view::npos) {
            const std::string_view room = objectId.substr(fascia + kFascia.size());
            if (room.find('/') != std::string_view::npos) {
                return std::nullopt;
            }
            if (std::optional<world::RoomId> parsed = parseRoom(room)) {
                return std::pair{*parsed, IdentitySignPlace::Hall};
            }
            return std::nullopt;
        }
        const std::size_t portal = objectId.find(kPortal);
        if (portal == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view rest = objectId.substr(portal + kPortal.size());
        const std::size_t slash = rest.find('/');
        if (slash == std::string_view::npos || !isPortalPiece(rest.substr(slash + 1))) {
            return std::nullopt;
        }
        if (std::optional<world::RoomId> parsed = parseRoom(rest.substr(0, slash))) {
            return std::pair{*parsed, IdentitySignPlace::Hall};
        }
        return std::nullopt;
    }
    // "<room>/fx:hero.sign", or a pack's further piece "<room>/fx:hero.sign.<n>".
    const std::size_t slash = objectId.find('/');
    if (slash == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view local = objectId.substr(slash + 1);
    if (!local.starts_with(kHeroSign) || (local.size() > kHeroSign.size() && local[kHeroSign.size()] != '.')) {
        return std::nullopt;
    }
    if (std::optional<world::RoomId> parsed = parseRoom(objectId.substr(0, slash))) {
        return std::pair{*parsed, IdentitySignPlace::Hero};
    }
    return std::nullopt;
}

std::vector<IdentitySignTarget> identitySignTargets(const scene::SceneDescription& description) {
    std::vector<IdentitySignTarget> targets;
    for (const scene::SceneObject& object : description.objects) {
        const auto picture = object.texturesByRole.find(std::string(kSignPictureRole));
        if (picture == object.texturesByRole.end() || picture->second.kind != scene::TextureSource::Kind::Pattern) {
            continue;
        }
        const std::string& pattern = picture->second.pattern;
        if (pattern != "neon" && pattern != "sign") {
            continue;
        }
        const auto room = identitySignRoom(object.id);
        if (!room) {
            continue;
        }
        targets.push_back(IdentitySignTarget{object.id, room->first, room->second,
                                             pattern == "neon" ? assets::IdentitySignStyle::Neon
                                                               : assets::IdentitySignStyle::Lit});
    }
    return targets;
}

bool hasIdentity(const SignSystem& system) noexcept {
    return !system.logo.empty() || (!system.esdeName.empty() && !system.name.empty() && system.name != system.esdeName);
}

std::optional<IdentitySignContent> identitySignContent(const world::RoomId& room, IdentitySignPlace place,
                                                       std::span<const catalog::SystemName> sections,
                                                       const SignSystemLookup& lookup) {
    if (!lookup) {
        return std::nullopt;
    }
    const std::string number = room.isExpansion() ? std::to_string(room.index()) : std::string();
    if (room.kind() == world::RoomIdKind::System) {
        const std::optional<SignSystem> system = lookup(room.system());
        if (!system || !hasIdentity(*system)) {
            return std::nullopt;
        }
        // A logo says the name already (theirs are wordmarks): the name only without one.
        IdentitySignContent content;
        if (!system->logo.empty()) {
            content.logos.push_back(system->logo);
            content.name = number;
        } else {
            content.name = number.empty() ? system->name : system->name + " " + number;
        }
        return content;
    }
    if (place == IdentitySignPlace::Hero) {
        return std::nullopt;
    }
    IdentitySignContent content;
    for (const catalog::SystemName& section : sections) {
        if (content.logos.size() >= assets::kMaxSignLogos) {
            break;
        }
        if (const std::optional<SignSystem> system = lookup(section); system && !system->logo.empty()) {
            content.logos.push_back(system->logo);
        }
    }
    if (content.logos.empty()) {
        return std::nullopt;
    }
    content.name = number;
    return content;
}

} // namespace rv::app
