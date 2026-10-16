#include <rv/core/utf8.hpp>

#include <array>
#include <cstdint>
#include <utility>

#include <fmt/format.h>

namespace rv::core {
namespace {

static_assert(sizeof(wchar_t) == 2, "the UTF-16 conversions assume Windows' 16-bit wchar_t");

constexpr char32_t kReplacementCharacter = 0xFFFD;

/// Length of the well-formed sequence that starts at `i` (Unicode table 3-7), or 0 when it is malformed.
std::size_t sequenceLength(std::string_view text, std::size_t i) noexcept {
    const auto lead = static_cast<unsigned char>(text[i]);
    if (lead < 0x80) {
        return 1;
    }
    std::size_t length = 0;
    unsigned low = 0x80; // valid range of the second byte
    unsigned high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        low = lead == 0xE0 ? 0xA0 : low;   // overlong
        high = lead == 0xED ? 0x9F : high; // UTF-16 surrogates
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        low = lead == 0xF0 ? 0x90 : low;   // overlong
        high = lead == 0xF4 ? 0x8F : high; // above U+10FFFF
    } else {
        return 0; // a continuation byte without a lead byte, or C0, C1, F5..FF
    }
    if (text.size() - i < length) {
        return 0;
    }
    const unsigned second = static_cast<unsigned char>(text[i + 1]);
    if (second < low || second > high) {
        return 0;
    }
    for (std::size_t k = 2; k < length; ++k) {
        if ((static_cast<unsigned char>(text[i + k]) & 0xC0u) != 0x80u) {
            return 0;
        }
    }
    return length;
}

void appendScalar(std::string& out, char32_t codePoint) {
    if (codePoint < 0x80) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

/// UTF-16 to UTF-8 into `out`. An unpaired surrogate becomes U+FFFD when `lossy`; otherwise the conversion stops and
/// its code unit offset is returned.
std::optional<std::size_t> convertWide(std::wstring_view wide, std::string& out, bool lossy) {
    out.reserve(wide.size());
    for (std::size_t i = 0; i < wide.size(); ++i) {
        const auto unit = static_cast<char32_t>(static_cast<std::uint16_t>(wide[i]));
        if (unit < 0x80) {
            out.push_back(static_cast<char>(unit));
            continue;
        }
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < wide.size()) {
            const auto next = static_cast<char32_t>(static_cast<std::uint16_t>(wide[i + 1]));
            if (next >= 0xDC00 && next <= 0xDFFF) {
                appendScalar(out, 0x10000 + ((unit - 0xD800) << 10) + (next - 0xDC00));
                ++i;
                continue;
            }
        }
        if (unit >= 0xD800 && unit <= 0xDFFF) {
            if (!lossy) {
                return i;
            }
            appendScalar(out, kReplacementCharacter);
            continue;
        }
        appendScalar(out, unit);
    }
    return std::nullopt;
}

} // namespace

std::optional<std::size_t> findInvalidUtf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t length = sequenceLength(text, i);
        if (length == 0) {
            return i;
        }
        i += length;
    }
    return std::nullopt;
}

bool isValidUtf8(std::string_view text) noexcept {
    return !findInvalidUtf8(text).has_value();
}

void appendUtf8(std::string& out, char32_t codePoint) {
    const bool scalar = codePoint <= 0x10FFFF && (codePoint < 0xD800 || codePoint > 0xDFFF);
    appendScalar(out, scalar ? codePoint : kReplacementCharacter);
}

Expected<std::wstring> utf8ToWide(std::string_view utf8) {
    std::wstring wide;
    wide.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        if (lead < 0x80) {
            wide.push_back(static_cast<wchar_t>(lead));
            ++i;
            continue;
        }
        const std::size_t length = sequenceLength(utf8, i);
        if (length == 0) {
            return makeError(ErrorCode::InvalidArgument, fmt::format("malformed UTF-8 at byte offset {}", i));
        }
        char32_t codePoint = lead & (length == 2 ? 0x1Fu : length == 3 ? 0x0Fu : 0x07u);
        for (std::size_t k = 1; k < length; ++k) {
            codePoint = (codePoint << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3Fu);
        }
        if (codePoint >= 0x10000) {
            codePoint -= 0x10000;
            wide.push_back(static_cast<wchar_t>(0xD800 + (codePoint >> 10)));
            wide.push_back(static_cast<wchar_t>(0xDC00 + (codePoint & 0x3FF)));
        } else {
            wide.push_back(static_cast<wchar_t>(codePoint));
        }
        i += length;
    }
    return wide;
}

Expected<std::string> wideToUtf8(std::wstring_view wide) {
    std::string utf8;
    if (const auto unpaired = convertWide(wide, utf8, false)) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("unpaired UTF-16 surrogate at code unit offset {}", *unpaired));
    }
    return utf8;
}

std::string wideToUtf8Lossy(std::wstring_view wide) {
    std::string utf8;
    convertWide(wide, utf8, true);
    return utf8;
}

Expected<std::filesystem::path> tryPathFromUtf8(std::string_view utf8) {
    auto wide = utf8ToWide(utf8);
    if (!wide) {
        return propagate(std::move(wide.error()));
    }
    return std::filesystem::path(std::move(*wide));
}

Expected<std::string> tryPathToUtf8(const std::filesystem::path& path) {
    return wideToUtf8(path.native());
}

std::string pathToUtf8Lossy(const std::filesystem::path& path) {
    return wideToUtf8Lossy(path.native());
}

namespace {

/// The characters of Windows-1252's bytes 0x80 to 0x9F (0xA0 to 0xFF are those of Latin-1). The five bytes it leaves
/// undefined stand for the C1 controls of the same value, as Windows and the WHATWG encoding standard read them.
constexpr std::array<char16_t, 32> kWindows1252High{
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, // 80 to 87
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F, // 88 to 8F
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, // 90 to 97
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178, // 98 to 9F
};

char32_t windows1252CodePoint(unsigned char byte) noexcept {
    return byte >= 0x80 && byte <= 0x9F ? kWindows1252High[byte - 0x80] : static_cast<char32_t>(byte);
}

} // namespace

std::string windows1252ToUtf8(std::string_view bytes) {
    std::string utf8;
    utf8.reserve(bytes.size());
    for (const char c : bytes) {
        appendScalar(utf8, windows1252CodePoint(static_cast<unsigned char>(c)));
    }
    return utf8;
}

std::string utf8WithWindows1252Fallback(std::string_view bytes, std::size_t* fallbackBytes) {
    std::size_t fallback = 0;
    std::string utf8;
    utf8.reserve(bytes.size());
    std::size_t i = 0;
    while (i < bytes.size()) {
        const std::size_t length = sequenceLength(bytes, i);
        if (length == 0) {
            appendScalar(utf8, windows1252CodePoint(static_cast<unsigned char>(bytes[i])));
            ++fallback;
            ++i;
            continue;
        }
        utf8.append(bytes.substr(i, length));
        i += length;
    }
    if (fallbackBytes != nullptr) {
        *fallbackBytes = fallback;
    }
    return utf8;
}

} // namespace rv::core
