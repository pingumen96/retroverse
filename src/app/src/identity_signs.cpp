#include <rv/app/identity_signs.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

#include <rv/core/math.hpp>

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

std::string clampToAccentBand(const glm::vec3& srgb) {
    const glm::vec3 c = glm::clamp(srgb, glm::vec3(0.0F), glm::vec3(1.0F));
    const float high = std::max({c.r, c.g, c.b});
    const float low = std::min({c.r, c.g, c.b});
    const float chroma = high - low;
    // The hue, in sixths of a turn.
    float hue = 0.0F;
    if (chroma > 0.0F) {
        if (high == c.r) {
            hue = std::fmod((c.g - c.b) / chroma + 6.0F, 6.0F);
        } else if (high == c.g) {
            hue = (c.b - c.r) / chroma + 2.0F;
        } else {
            hue = (c.r - c.g) / chroma + 4.0F;
        }
    }
    const float saturation =
        std::clamp(high > 0.0F ? chroma / high : 0.0F, kAccentMinSaturation, kAccentMaxSaturation);
    const float value = std::clamp(high, kAccentMinValue, kAccentMaxValue);
    // Back from HSV.
    const float span = value * saturation;
    const float x = span * (1.0F - std::abs(std::fmod(hue, 2.0F) - 1.0F));
    glm::vec3 rgb(0.0F);
    switch (static_cast<int>(hue) % 6) {
    case 0:
        rgb = {span, x, 0.0F};
        break;
    case 1:
        rgb = {x, span, 0.0F};
        break;
    case 2:
        rgb = {0.0F, span, x};
        break;
    case 3:
        rgb = {0.0F, x, span};
        break;
    case 4:
        rgb = {x, 0.0F, span};
        break;
    default:
        rgb = {span, 0.0F, x};
        break;
    }
    rgb += glm::vec3(value - span);
    const auto byte = [](float channel) {
        return static_cast<unsigned>(std::lround(std::clamp(channel, 0.0F, 1.0F) * 255.0F));
    };
    char text[8];
    std::snprintf(text, sizeof(text), "#%02X%02X%02X", byte(rgb.r), byte(rgb.g), byte(rgb.b));
    return std::string(text);
}

std::optional<std::string> systemAccent(std::span<const std::string> palette) {
    std::optional<glm::vec3> best;
    float bestChroma = -1.0F;
    for (const std::string& entry : palette) {
        const std::string hex = !entry.empty() && entry.front() != '#' ? "#" + entry : entry;
        const auto parsed = core::parseHexColor(hex);
        if (!parsed) {
            continue;
        }
        const glm::vec3 colour(parsed->r, parsed->g, parsed->b);
        const float chroma = std::max({colour.r, colour.g, colour.b}) - std::min({colour.r, colour.g, colour.b});
        if (chroma > bestChroma) {
            bestChroma = chroma;
            best = colour;
        }
    }
    if (!best || bestChroma < kAccentMinChroma) {
        return std::nullopt;
    }
    return clampToAccentBand(*best);
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
