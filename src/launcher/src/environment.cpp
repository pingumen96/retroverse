#include <rv/launcher/environment.hpp>

#include <algorithm>
#include <cstdlib>

#include <rv/core/utf8.hpp>
#include <rv/launcher/command_line.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

std::optional<std::string> processEnvironmentVariable(std::string_view name) {
    if (name.empty() || name.find_first_of(std::string_view("=\0", 2)) != std::string_view::npos) {
        return std::nullopt;
    }
#if defined(_WIN32)
    const auto wideName = core::utf8ToWide(name);
    if (!wideName) {
        return std::nullopt;
    }
    std::wstring value(64, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetEnvironmentVariableW(wideName->c_str(), value.data(), static_cast<DWORD>(value.size()));
        if (length == 0) {
            // 0 is both "not set" and "set to an empty value": only the error code tells them apart.
            if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
                return std::nullopt;
            }
            return std::string();
        }
        if (length < value.size()) {
            value.resize(length);
            return core::wideToUtf8Lossy(value);
        }
        value.resize(length); // the size it needs, its terminator included; the variable may change meanwhile
    }
    return std::nullopt;
#else
    const char* value = std::getenv(std::string(name).c_str());
    return value == nullptr ? std::nullopt : std::optional<std::string>(value);
#endif
}

} // namespace rv::launcher
