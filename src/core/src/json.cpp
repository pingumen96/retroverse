#include <rv/core/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

namespace rv::core {
namespace {

template <int N>
Expected<glm::vec<N, float, glm::defaultp>> jsonToVector(const nlohmann::json& value, std::string_view context) {
    if (!value.is_array() || value.size() != static_cast<std::size_t>(N)) {
        return makeError(ErrorCode::ParseError, "expected an array of " + std::to_string(N) + " numbers",
                         std::string(context));
    }
    glm::vec<N, float, glm::defaultp> result(0.0f);
    for (int i = 0; i < N; ++i) {
        const auto& element = value[static_cast<std::size_t>(i)];
        if (!element.is_number()) {
            return makeError(ErrorCode::ParseError, "array element " + std::to_string(i) + " is not a number",
                             std::string(context));
        }
        result[i] = element.get<float>();
    }
    return result;
}

} // namespace

Expected<nlohmann::json> parseJson(std::string_view text, std::string_view context) {
    try {
        return nlohmann::json::parse(text.begin(), text.end(), /*callback*/ nullptr, /*allow_exceptions*/ true,
                                     /*ignore_comments*/ true);
    } catch (const nlohmann::json::exception& e) {
        // parse_error for syntax errors, but also out_of_range (406) for number literals that overflow a double:
        // the DOM parser rethrows the latter as-is, so catching only parse_error would let it escape.
        return makeError(ErrorCode::ParseError, e.what(), std::string(context));
    }
}

Expected<nlohmann::json> readJsonFile(const std::filesystem::path& path) {
    auto text = readTextFile(path);
    if (!text) {
        return propagate(text.error());
    }
    return parseJson(*text, pathToUtf8Lossy(path));
}

Expected<glm::vec2> jsonToVec2(const nlohmann::json& value, std::string_view context) {
    return jsonToVector<2>(value, context);
}

Expected<glm::vec3> jsonToVec3(const nlohmann::json& value, std::string_view context) {
    return jsonToVector<3>(value, context);
}

Expected<glm::vec4> jsonToVec4(const nlohmann::json& value, std::string_view context) {
    return jsonToVector<4>(value, context);
}

nlohmann::json toJson(const glm::vec2& value) {
    return nlohmann::json::array({value.x, value.y});
}

nlohmann::json toJson(const glm::vec3& value) {
    return nlohmann::json::array({value.x, value.y, value.z});
}

nlohmann::json toJson(const glm::vec4& value) {
    return nlohmann::json::array({value.x, value.y, value.z, value.w});
}

} // namespace rv::core
