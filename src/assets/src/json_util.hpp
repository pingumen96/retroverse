#pragma once

// Internal helpers for strict JSON reading (material library, prefab manifests). Not part of the public API.
//
// Contexts follow the JSON-pointer style used across Retroverse: "<file>:/materials/MAT_Wood_Oak/roughness".
// Callers start from "<file>:" and append tokens with pointerChild().

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/expected.hpp>

namespace rv::assets::detail {

/// Appends one reference token ("~" and "/" escaped as in RFC 6901): ("a.json:", "x") -> "a.json:/x".
[[nodiscard]] std::string pointerChild(std::string_view base, std::string_view token);
[[nodiscard]] std::string pointerChild(std::string_view base, std::size_t index);

/// Readable name of a JSON value type for error messages ("string", "number", ...).
[[nodiscard]] std::string_view jsonTypeName(const nlohmann::json& value) noexcept;

[[nodiscard]] Status requireObject(const nlohmann::json& value, std::string_view context);

/// Fails on the first key of `object` that is not listed in `allowed`; the error context names the key.
[[nodiscard]] Status rejectUnknownKeys(const nlohmann::json& object, std::initializer_list<std::string_view> allowed,
                                       std::string_view context);

[[nodiscard]] Expected<std::string> readString(const nlohmann::json& value, std::string_view context);
/// Like readString(), but JSON null reads as an empty string (optional fields written as null).
[[nodiscard]] Expected<std::string> readNullableString(const nlohmann::json& value, std::string_view context);
[[nodiscard]] Expected<bool> readBool(const nlohmann::json& value, std::string_view context);
/// Finite number representable as float.
[[nodiscard]] Expected<float> readNumber(const nlohmann::json& value, std::string_view context);
/// Finite number in [minValue, maxValue].
[[nodiscard]] Expected<float> readNumberInRange(const nlohmann::json& value, float minValue, float maxValue,
                                                std::string_view context);
/// Integral number representable as int.
[[nodiscard]] Expected<int> readInteger(const nlohmann::json& value, std::string_view context);
[[nodiscard]] Expected<std::vector<std::string>> readStringArray(const nlohmann::json& value, std::string_view context);

/// When `object` has `key`, reads it with `reader(value, "<context>/<key>")` (one of the functions above or a
/// lambda with the same shape) into `out`; a missing key leaves `out` untouched.
template <class T, class Reader>
[[nodiscard]] Status readOptional(const nlohmann::json& object, std::string_view key, std::string_view context,
                                  Reader&& reader, T& out) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return ok();
    }
    auto value = reader(*it, pointerChild(context, key));
    if (!value) {
        return propagate(value.error());
    }
    out = static_cast<T>(std::move(*value));
    return ok();
}

} // namespace rv::assets::detail
