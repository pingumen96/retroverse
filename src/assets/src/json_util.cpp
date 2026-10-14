#include "json_util.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace rv::assets::detail {

std::string pointerChild(std::string_view base, std::string_view token) {
    std::string result(base);
    result += '/';
    for (const char c : token) {
        if (c == '~') {
            result += "~0";
        } else if (c == '/') {
            result += "~1";
        } else {
            result += c;
        }
    }
    return result;
}

std::string pointerChild(std::string_view base, std::size_t index) {
    return pointerChild(base, std::to_string(index));
}

std::string_view jsonTypeName(const nlohmann::json& value) noexcept {
    switch (value.type()) {
    case nlohmann::json::value_t::null:
        return "null";
    case nlohmann::json::value_t::object:
        return "object";
    case nlohmann::json::value_t::array:
        return "array";
    case nlohmann::json::value_t::string:
        return "string";
    case nlohmann::json::value_t::boolean:
        return "boolean";
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned:
    case nlohmann::json::value_t::number_float:
        return "number";
    case nlohmann::json::value_t::binary:
        return "binary";
    case nlohmann::json::value_t::discarded:
        return "discarded";
    }
    return "unknown";
}

namespace {

tl::unexpected<Error> typeError(const nlohmann::json& value, std::string_view expected, std::string_view context) {
    return makeError(ErrorCode::ParseError,
                     "expected " + std::string(expected) + ", found " + std::string(jsonTypeName(value)),
                     std::string(context));
}

} // namespace

Status requireObject(const nlohmann::json& value, std::string_view context) {
    if (!value.is_object()) {
        return typeError(value, "an object", context);
    }
    return ok();
}

Status rejectUnknownKeys(const nlohmann::json& object, std::initializer_list<std::string_view> allowed,
                         std::string_view context) {
    for (const auto& item : object.items()) {
        bool known = false;
        for (const std::string_view key : allowed) {
            if (item.key() == key) {
                known = true;
                break;
            }
        }
        if (!known) {
            return makeError(ErrorCode::ParseError, "unknown key \"" + item.key() + "\"",
                             pointerChild(context, item.key()));
        }
    }
    return ok();
}

Expected<std::string> readString(const nlohmann::json& value, std::string_view context) {
    if (!value.is_string()) {
        return typeError(value, "a string", context);
    }
    return value.get<std::string>();
}

Expected<std::string> readNullableString(const nlohmann::json& value, std::string_view context) {
    if (value.is_null()) {
        return std::string();
    }
    if (!value.is_string()) {
        return typeError(value, "a string or null", context);
    }
    return value.get<std::string>();
}

Expected<bool> readBool(const nlohmann::json& value, std::string_view context) {
    if (!value.is_boolean()) {
        return typeError(value, "a boolean", context);
    }
    return value.get<bool>();
}

Expected<float> readNumber(const nlohmann::json& value, std::string_view context) {
    if (!value.is_number()) {
        return typeError(value, "a number", context);
    }
    const double number = value.get<double>();
    if (!std::isfinite(number) || std::fabs(number) > static_cast<double>(std::numeric_limits<float>::max())) {
        return makeError(ErrorCode::OutOfRange, "number is not finite", std::string(context));
    }
    return static_cast<float>(number);
}

Expected<float> readNumberInRange(const nlohmann::json& value, float minValue, float maxValue,
                                  std::string_view context) {
    auto number = readNumber(value, context);
    if (!number) {
        return number;
    }
    if (*number < minValue || *number > maxValue) {
        return makeError(ErrorCode::OutOfRange,
                         "value " + std::to_string(*number) + " is outside [" + std::to_string(minValue) + ", " +
                             std::to_string(maxValue) + "]",
                         std::string(context));
    }
    return number;
}

Expected<int> readInteger(const nlohmann::json& value, std::string_view context) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            return makeError(ErrorCode::OutOfRange, "integer is too large", std::string(context));
        }
        return static_cast<int>(number);
    }
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) {
            return makeError(ErrorCode::OutOfRange, "integer is out of range", std::string(context));
        }
        return static_cast<int>(number);
    }
    return typeError(value, "an integer", context);
}

Expected<std::vector<std::string>> readStringArray(const nlohmann::json& value, std::string_view context) {
    if (!value.is_array()) {
        return typeError(value, "an array of strings", context);
    }
    std::vector<std::string> strings;
    strings.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        auto element = readString(value[i], pointerChild(context, i));
        if (!element) {
            return propagate(element.error());
        }
        strings.push_back(std::move(*element));
    }
    return strings;
}

} // namespace rv::assets::detail
