#include <rv/launcher/environment.hpp>

#include <algorithm>

#include <rv/launcher/command_line.hpp>

namespace rv::launcher {
namespace {

[[nodiscard]] char upperAscii(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

/// Orders names as their upper-case form: case-insensitive for ASCII letters, by byte (= by code point) otherwise.
[[nodiscard]] bool nameLess(std::string_view a, std::string_view b) noexcept {
    const std::size_t common = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < common; ++i) {
        const auto ca = static_cast<unsigned char>(upperAscii(a[i]));
        const auto cb = static_cast<unsigned char>(upperAscii(b[i]));
        if (ca != cb) {
            return ca < cb;
        }
    }
    return a.size() < b.size();
}

} // namespace

bool sameVariableName(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (upperAscii(a[i]) != upperAscii(b[i])) {
            return false;
        }
    }
    return true;
}

Status validateOverride(const EnvironmentOverride& override) {
    if (override.name.empty()) {
        return makeError(ErrorCode::ValidationFailed, "an environment variable has no name");
    }
    if (override.name.find('=') != std::string::npos) {
        return makeError(ErrorCode::ValidationFailed, "an environment variable name holds '='", override.name);
    }
    if (override.name.find('\0') != std::string::npos || !isValidUtf8(override.name)) {
        return makeError(ErrorCode::ValidationFailed,
                         "an environment variable name holds a NUL character or malformed UTF-8", "environment");
    }
    if (override.value &&
        (override.value->find('\0') != std::string::npos || !isValidUtf8(*override.value))) {
        return makeError(ErrorCode::ValidationFailed,
                         "an environment variable value holds a NUL character or malformed UTF-8", override.name);
    }
    return ok();
}

std::vector<EnvironmentVariable> mergeEnvironment(std::span<const EnvironmentVariable> inherited,
                                                  std::span<const EnvironmentOverride> overrides) {
    std::vector<EnvironmentVariable> merged(inherited.begin(), inherited.end());
    for (const EnvironmentOverride& override : overrides) {
        const auto existing = std::find_if(merged.begin(), merged.end(), [&](const EnvironmentVariable& variable) {
            return sameVariableName(variable.name, override.name);
        });
        if (!override.value) {
            if (existing != merged.end()) {
                merged.erase(existing);
            }
        } else if (existing != merged.end()) {
            existing->value = *override.value;
        } else {
            merged.push_back(EnvironmentVariable{override.name, *override.value});
        }
    }
    std::stable_sort(merged.begin(), merged.end(), [](const EnvironmentVariable& a, const EnvironmentVariable& b) {
        return nameLess(a.name, b.name);
    });
    return merged;
}

} // namespace rv::launcher
