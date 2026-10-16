#include <rv/core/case_fold.hpp>

#include <climits>
#include <utility>

#include <windows.h>

#include <fmt/format.h>

#include <rv/core/system_error.hpp>
#include <rv/core/utf8.hpp>

namespace rv::core {
namespace {

bool isAscii(std::string_view text) noexcept {
    for (const char c : text) {
        if (static_cast<unsigned char>(c) >= 0x80) {
            return false;
        }
    }
    return true;
}

bool isSurrogate(wchar_t unit) noexcept {
    return unit >= 0xD800 && unit <= 0xDFFF;
}

/// Upper-cases `run`, which holds no surrogate, in place in `out` (the same length).
Status upperCaseRun(std::wstring_view run, wchar_t* out) {
    if (run.size() > static_cast<std::size_t>(INT_MAX)) {
        return makeError(ErrorCode::InvalidArgument, "text too long to change its case");
    }
    const int length = static_cast<int>(run.size());
    // File-system casing: the invariant locale's simple upper case, without LCMAP_LINGUISTIC_CASING. On text without
    // surrogates it maps each code unit to one (the unit tests check every one against CompareStringOrdinal).
    const int written =
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, run.data(), length, out, length, nullptr, nullptr, 0);
    if (written != length) {
        return makeError(ErrorCode::PlatformError,
                         written == 0 ? "LCMapStringEx failed: " + lastSystemErrorMessage()
                                      : "LCMapStringEx changed the length of the text");
    }
    return ok();
}

} // namespace

Expected<std::wstring> fileSystemUpperCase(std::wstring_view utf16) {
    // NTFS and exFAT upper-case names one UTF-16 code unit at a time with a table of 65,536 entries, so the letters
    // outside the BMP (surrogate pairs) never change case there, while LCMapStringEx would map them (U+10428 to
    // U+10400): only the runs between surrogates go through it, surrogates are copied as they are.
    std::wstring upper(utf16);
    std::size_t start = 0;
    while (start < utf16.size()) {
        if (isSurrogate(utf16[start])) {
            ++start;
            continue;
        }
        std::size_t end = start;
        while (end < utf16.size() && !isSurrogate(utf16[end])) {
            ++end;
        }
        if (auto done = upperCaseRun(utf16.substr(start, end - start), upper.data() + start); !done) {
            return propagate(std::move(done.error()));
        }
        start = end;
    }
    return upper;
}

Expected<std::string> fileSystemUpperCase(std::string_view utf8) {
    if (isAscii(utf8)) {
        // The table maps nothing in ASCII but a to z (pinned against the system call by the unit tests).
        std::string upper(utf8);
        for (char& c : upper) {
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        return upper;
    }
    auto wide = utf8ToWide(utf8);
    if (!wide) {
        return propagate(std::move(wide.error()));
    }
    auto upper = fileSystemUpperCase(std::wstring_view(*wide));
    if (!upper) {
        return propagate(std::move(upper.error()));
    }
    return wideToUtf8(*upper);
}

std::string casingTableVersion() {
    NLSVERSIONINFOEX info{};
    info.dwNLSVersionInfoSize = sizeof(info);
    if (!GetNLSVersionEx(COMPARE_STRING, LOCALE_NAME_INVARIANT, &info)) {
        return "unknown";
    }
    return fmt::format("{:x}.{:x}", info.dwNLSVersion, info.dwDefinedVersion);
}

} // namespace rv::core
