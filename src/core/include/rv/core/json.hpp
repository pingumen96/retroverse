#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/core/expected.hpp>

namespace rv::core {

/// Parses JSON text. Comments are accepted (hand-written manifests and scene files). Errors carry the byte
/// position reported by nlohmann::json and `context` (usually the file path).
[[nodiscard]] Expected<nlohmann::json> parseJson(std::string_view text, std::string_view context);

/// Reads and parses a JSON file (size-limited, see readTextFile).
[[nodiscard]] Expected<nlohmann::json> readJsonFile(const std::filesystem::path& path);

/// Strict array conversions: `[x, y]`, `[x, y, z]`, `[x, y, z, w]` of numbers. `context` should describe the
/// location, e.g. "m0_testroom.json:/lights/0/position".
[[nodiscard]] Expected<glm::vec2> jsonToVec2(const nlohmann::json& value, std::string_view context);
[[nodiscard]] Expected<glm::vec3> jsonToVec3(const nlohmann::json& value, std::string_view context);
[[nodiscard]] Expected<glm::vec4> jsonToVec4(const nlohmann::json& value, std::string_view context);

[[nodiscard]] nlohmann::json toJson(const glm::vec2& value);
[[nodiscard]] nlohmann::json toJson(const glm::vec3& value);
[[nodiscard]] nlohmann::json toJson(const glm::vec4& value);

} // namespace rv::core
