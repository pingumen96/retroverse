#include <rv/world/ids.hpp>

#include <utility>

namespace rv::world {
namespace {

constexpr std::string_view kRoomPrefix = "room:";
constexpr std::string_view kFixtureSeparator = "/fx:";
constexpr std::string_view kLevelSeparator = "/l:";
constexpr std::string_view kSlotSeparator = "/s:";

bool isLower(char c) noexcept {
    return c >= 'a' && c <= 'z';
}

bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

tl::unexpected<Error> invalid(std::string_view what, std::string_view text) {
    return makeError(ErrorCode::InvalidArgument, "not a valid " + std::string(what), std::string(text));
}

/// Splits "<head><separator><tail>" at the last separator.
std::optional<std::pair<std::string_view, std::string_view>> splitLast(std::string_view text,
                                                                       std::string_view separator) {
    const std::size_t at = text.rfind(separator);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    return std::pair{text.substr(0, at), text.substr(at + separator.size())};
}

std::string decimal(std::uint32_t value) {
    return std::to_string(value);
}

} // namespace

namespace detail {

std::optional<std::uint32_t> parseDecimal(std::string_view text, std::uint32_t maximum) noexcept {
    if (text.empty() || text.size() > 10 || (text.size() > 1 && text.front() == '0')) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (!isDigit(c)) {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (value > maximum) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

} // namespace detail

bool isValidSocketName(std::string_view text) noexcept {
    if (text.empty() || text.size() > kMaxSocketNameLength || !isLower(text.front())) {
        return false;
    }
    for (const char c : text) {
        if (!isLower(c) && !isDigit(c) && c != '-') {
            return false;
        }
    }
    return true;
}

bool isValidPrefabKind(std::string_view text) noexcept {
    return isValidDataId(text);
}

bool isValidDataId(std::string_view text) noexcept {
    if (text.empty() || text.size() > 64 || !isLower(text.front())) {
        return false;
    }
    for (const char c : text) {
        if (!isLower(c) && !isDigit(c) && c != '_') {
            return false;
        }
    }
    return true;
}

bool isValidRuleName(std::string_view text) noexcept {
    const std::size_t slash = text.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash > 64 || !isLower(text.front())) {
        return false;
    }
    for (const char c : text.substr(0, slash)) {
        if (!isLower(c) && !isDigit(c) && c != '_' && c != '-') {
            return false;
        }
    }
    const auto version = detail::parseDecimal(text.substr(slash + 1), 999'999);
    return version.has_value() && *version >= 1;
}

// RoomId

Expected<RoomId> RoomId::forSystem(const catalog::SystemName& system) {
    if (system.empty()) {
        return makeError(ErrorCode::InvalidArgument, "a room needs a system");
    }
    RoomId room;
    room.m_system = system;
    return room;
}

Expected<RoomId> RoomId::parse(std::string_view text) {
    if (!text.starts_with(kRoomPrefix)) {
        return invalid("room id", text);
    }
    // A SystemName holds neither '/' nor ':', so the room id ends where a fixture's separator starts.
    auto system = catalog::SystemName::parse(text.substr(kRoomPrefix.size()));
    if (!system) {
        return invalid("room id", text);
    }
    return forSystem(*system);
}

std::string RoomId::toString() const {
    return empty() ? std::string() : std::string(kRoomPrefix) + m_system.str();
}

// FixtureId

Expected<FixtureId> FixtureId::make(const RoomId& room, std::string_view socket, std::string_view part) {
    if (room.empty() || !isValidSocketName(socket) || (!part.empty() && !isValidSocketName(part))) {
        return makeError(ErrorCode::InvalidArgument, "not a valid fixture",
                         room.toString() + std::string(kFixtureSeparator) + std::string(socket) +
                             (part.empty() ? std::string() : "." + std::string(part)));
    }
    FixtureId fixture;
    fixture.m_room = room;
    fixture.m_socket = socket;
    fixture.m_part = part;
    return fixture;
}

Expected<FixtureId> FixtureId::parse(std::string_view text) {
    const std::size_t at = text.find(kFixtureSeparator);
    if (at == std::string_view::npos) {
        return invalid("fixture id", text);
    }
    auto room = RoomId::parse(text.substr(0, at));
    if (!room) {
        return invalid("fixture id", text);
    }
    std::string_view socket = text.substr(at + kFixtureSeparator.size());
    std::string_view part;
    if (const std::size_t dot = socket.find('.'); dot != std::string_view::npos) {
        part = socket.substr(dot + 1);
        socket = socket.substr(0, dot);
        if (part.empty()) {
            return invalid("fixture id", text);
        }
    }
    auto fixture = make(*room, socket, part);
    if (!fixture) {
        return invalid("fixture id", text);
    }
    return fixture;
}

std::string FixtureId::localText() const {
    if (empty()) {
        return {};
    }
    std::string text = "fx:" + m_socket;
    if (!m_part.empty()) {
        text += "." + m_part;
    }
    return text;
}

std::string FixtureId::toString() const {
    return empty() ? std::string() : m_room.toString() + "/" + localText();
}

// LevelId

Expected<LevelId> LevelId::make(const FixtureId& fixture, std::uint32_t level) {
    if (fixture.empty() || level > kMaxLevel) {
        return makeError(ErrorCode::InvalidArgument, "not a valid level",
                         fixture.toString() + std::string(kLevelSeparator) + decimal(level));
    }
    LevelId id;
    id.m_fixture = fixture;
    id.m_level = level;
    return id;
}

Expected<LevelId> LevelId::parse(std::string_view text) {
    const auto parts = splitLast(text, kLevelSeparator);
    if (!parts) {
        return invalid("level id", text);
    }
    auto fixture = FixtureId::parse(parts->first);
    const auto level = detail::parseDecimal(parts->second, kMaxLevel);
    if (!fixture || !level) {
        return invalid("level id", text);
    }
    return make(*fixture, *level);
}

std::string LevelId::localText() const {
    return empty() ? std::string() : m_fixture.localText() + std::string(kLevelSeparator) + decimal(m_level);
}

std::string LevelId::toString() const {
    return empty() ? std::string() : m_fixture.toString() + std::string(kLevelSeparator) + decimal(m_level);
}

// SlotId

Expected<SlotId> SlotId::make(const FixtureId& fixture, std::uint32_t level, std::uint32_t index) {
    if (fixture.empty() || level > kMaxLevel || index > kMaxSlotIndex) {
        return makeError(ErrorCode::InvalidArgument, "not a valid slot",
                         fixture.toString() + std::string(kSlotSeparator) + decimal(level) + "." + decimal(index));
    }
    SlotId slot;
    slot.m_fixture = fixture;
    slot.m_level = level;
    slot.m_index = index;
    return slot;
}

Expected<SlotId> SlotId::parse(std::string_view text) {
    const auto parts = splitLast(text, kSlotSeparator);
    if (!parts) {
        return invalid("slot id", text);
    }
    const std::size_t dot = parts->second.find('.');
    if (dot == std::string_view::npos) {
        return invalid("slot id", text);
    }
    auto fixture = FixtureId::parse(parts->first);
    const auto level = detail::parseDecimal(parts->second.substr(0, dot), kMaxLevel);
    const auto index = detail::parseDecimal(parts->second.substr(dot + 1), kMaxSlotIndex);
    if (!fixture || !level || !index) {
        return invalid("slot id", text);
    }
    return make(*fixture, *level, *index);
}

LevelId SlotId::levelId() const {
    if (empty()) {
        return {};
    }
    // Valid by construction: the slot's own fixture and level.
    return *LevelId::make(m_fixture, m_level);
}

std::string SlotId::toString() const {
    return empty() ? std::string()
                   : m_fixture.toString() + std::string(kSlotSeparator) + decimal(m_level) + "." + decimal(m_index);
}

} // namespace rv::world
