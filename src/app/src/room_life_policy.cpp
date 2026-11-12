#include <rv/app/room_life.hpp>

#include <cmath>

namespace rv::app {
namespace {

/// The two digits at `text[at]` as a number below `limit`, nothing otherwise.
std::optional<int> twoDigits(std::string_view text, std::size_t at, int limit) {
    if (at + 2 > text.size() || text[at] < '0' || text[at] > '9' || text[at + 1] < '0' || text[at + 1] > '9') {
        return std::nullopt;
    }
    const int value = (text[at] - '0') * 10 + (text[at + 1] - '0');
    return value < limit ? std::optional<int>(value) : std::nullopt;
}

} // namespace

std::optional<double> parseClockTime(std::string_view text) {
    if (text.size() != 5 && text.size() != 8) {
        return std::nullopt;
    }
    const std::optional<int> hours = twoDigits(text, 0, 24);
    const std::optional<int> minutes = text[2] == ':' ? twoDigits(text, 3, 60) : std::nullopt;
    std::optional<int> seconds = 0;
    if (text.size() == 8) {
        seconds = text[5] == ':' ? twoDigits(text, 6, 60) : std::nullopt;
    }
    if (!hours || !minutes || !seconds) {
        return std::nullopt;
    }
    return static_cast<double>(*hours * 3600 + *minutes * 60 + *seconds);
}

double clockAfter(double startSeconds, double seconds) noexcept {
    const double value = std::fmod(startSeconds + (std::isfinite(seconds) ? seconds : 0.0), scene::kSecondsPerDay);
    return value < 0.0 ? value + scene::kSecondsPerDay : value;
}

float lifeAngle(const scene::ObjectMotion& motion, double seconds, double clockSeconds, bool reduceMotion) noexcept {
    if (reduceMotion &&
        (motion.kind == scene::ObjectMotion::Kind::Spin || motion.kind == scene::ObjectMotion::Kind::ClockSecond)) {
        return 0.0f;
    }
    return scene::motionAngle(motion, seconds, clockSeconds);
}

} // namespace rv::app
