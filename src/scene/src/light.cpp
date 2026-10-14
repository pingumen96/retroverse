#include <rv/scene/light.hpp>

namespace rv::scene {

std::string_view toString(LightType type) noexcept {
    switch (type) {
    case LightType::Point:
        return "point";
    case LightType::Spot:
        return "spot";
    case LightType::Directional:
        return "directional";
    }
    return "point";
}

std::optional<LightType> lightTypeFromString(std::string_view name) noexcept {
    for (const LightType type : {LightType::Point, LightType::Spot, LightType::Directional}) {
        if (toString(type) == name) {
            return type;
        }
    }
    return std::nullopt;
}

} // namespace rv::scene
