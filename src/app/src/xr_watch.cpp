#include <rv/app/xr_watch.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace rv::app {

namespace {

[[nodiscard]] char lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool equalNoCase(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

/// The id's last "{...}" part: the endpoint's own GUID.
[[nodiscard]] std::string_view lastBraced(std::string_view id) noexcept {
    const std::size_t open = id.rfind('{');
    if (open == std::string_view::npos || id.empty() || id.back() != '}') {
        return {};
    }
    return id.substr(open);
}

} // namespace

bool HoldCommand::update(bool armed, bool held, float seconds) noexcept {
    if (!held) {
        m_held = 0.0f;
        m_spent = false;
        return false;
    }
    if (!armed) {
        // A hold that began (or went on) while the command could not fire counts only after a release.
        m_held = 0.0f;
        m_spent = true;
        return false;
    }
    if (m_spent) {
        return false;
    }
    m_held += std::isfinite(seconds) && seconds > 0.0f ? seconds : 0.0f;
    // A second of frames adds up to a hair under one in floats: a small allowance keeps 60 frames of 1/60 s a second.
    if (m_held >= kHoldSeconds - 1e-4f) {
        m_held = 0.0f;
        m_spent = true;
        return true;
    }
    return false;
}

bool sameAudioOutput(std::string_view a, std::string_view b) noexcept {
    if (a.empty() || b.empty()) {
        return false;
    }
    if (equalNoCase(a, b)) {
        return true;
    }
    // A bare GUID against a full endpoint id: the full id's own GUID.
    const std::string_view shorter = a.size() < b.size() ? a : b;
    const std::string_view longer = a.size() < b.size() ? b : a;
    const std::string_view guid = lastBraced(longer);
    return shorter.front() == '{' && !guid.empty() && equalNoCase(shorter, guid);
}

} // namespace rv::app
