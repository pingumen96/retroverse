#include <rv/catalog/ids.hpp>

#include <fmt/format.h>

#include <rv/core/utf8.hpp>

namespace rv::catalog {
namespace {

int hexDigitValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1; // uppercase digits too: the text form has one spelling
}

} // namespace

Expected<GameId> GameId::parse(std::string_view text) {
    const auto refuse = [&](std::string_view why) {
        return makeError(ErrorCode::InvalidArgument, "not a GameId: " + std::string(why), std::string(text));
    };
    if (text.size() != kTextLength || text.front() != 'g') {
        return refuse("expected 'g' and 16 lowercase hexadecimal digits");
    }
    std::uint64_t value = 0;
    for (const char c : text.substr(1)) {
        const int digit = hexDigitValue(c);
        if (digit < 0) {
            return refuse("expected 'g' and 16 lowercase hexadecimal digits");
        }
        value = (value << 4) | static_cast<std::uint64_t>(digit);
    }
    const auto id = fromValue(value);
    if (!id) {
        return refuse(value == 0 ? "0 is no game" : "the value exceeds 63 bits");
    }
    return *id;
}

std::string GameId::toString() const {
    return fmt::format("g{:016x}", m_value);
}

bool LibraryId::isValid(std::string_view text) noexcept {
    if (text.empty() || text.size() > kMaxLength) {
        return false;
    }
    for (const char c : text) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

Expected<LibraryId> LibraryId::parse(std::string_view text) {
    if (!isValid(text)) {
        return makeError(ErrorCode::InvalidArgument,
                         "not a library id: 1 to 32 characters among lowercase ASCII letters, digits and '-'",
                         std::string(text));
    }
    return LibraryId(std::string(text));
}

bool SystemName::isValid(std::string_view text) noexcept {
    if (text.empty() || text.size() > kMaxLength || text == "." || text == ".." || !core::isValidUtf8(text)) {
        return false;
    }
    if (text.front() == ' ' || text.back() == ' ' || text.back() == '.') {
        return false;
    }
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20) { // what Windows refuses in names; DEL is allowed
            return false;
        }
        switch (c) {
        case '<':
        case '>':
        case ':':
        case '"':
        case '/':
        case '\\':
        case '|':
        case '?':
        case '*':
            return false;
        default:
            break;
        }
    }
    return true;
}

Expected<SystemName> SystemName::parse(std::string_view text) {
    if (!isValid(text)) {
        return makeError(ErrorCode::InvalidArgument,
                         "not a system name: 1 to 255 bytes of UTF-8 that can be one folder name (no character "
                         "below U+0020, none of < > : \" / \\ | ? *, not . or .., no leading or trailing space, no "
                         "trailing dot)",
                         std::string(text));
    }
    return SystemName(std::string(text));
}

} // namespace rv::catalog
