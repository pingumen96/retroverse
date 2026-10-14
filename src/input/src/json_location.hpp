#pragma once

// Internal helpers shared by the JSON parsers of rv_input (binding files, input scripts). Not part of the API.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <rv/core/expected.hpp>

namespace rv::input::detail {

/// Location of a JSON value for error contexts: "<document>:<JSON pointer>" (RFC 6901 escaping).
class JsonLocation {
public:
    explicit JsonLocation(std::string_view document) : m_document(document) {}

    [[nodiscard]] JsonLocation child(std::string_view token) const {
        JsonLocation result = *this;
        result.m_pointer += '/';
        for (const char c : token) {
            if (c == '~') {
                result.m_pointer += "~0";
            } else if (c == '/') {
                result.m_pointer += "~1";
            } else {
                result.m_pointer += c;
            }
        }
        return result;
    }

    [[nodiscard]] JsonLocation child(std::size_t index) const { return child(std::to_string(index)); }

    [[nodiscard]] std::string str() const {
        if (m_pointer.empty()) {
            return std::string(m_document);
        }
        if (m_document.empty()) {
            return m_pointer;
        }
        return std::string(m_document) + ":" + m_pointer;
    }

private:
    std::string_view m_document;
    std::string m_pointer;
};

[[nodiscard]] inline tl::unexpected<Error> fail(ErrorCode code, std::string message, const JsonLocation& at) {
    return makeError(code, std::move(message), at.str());
}

[[nodiscard]] inline std::string inQuotes(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

} // namespace rv::input::detail
