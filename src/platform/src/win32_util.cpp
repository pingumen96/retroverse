#include "win32_util.hpp"

#include <climits>

#include <fmt/format.h>

namespace rv::platform::win32 {

Expected<std::wstring> widen(std::string_view utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    if (utf8.size() > static_cast<std::size_t>(INT_MAX)) {
        return makeError(ErrorCode::InvalidArgument, "text too long to convert to UTF-16");
    }
    const int length = static_cast<int>(utf8.size());
    const int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, nullptr, 0);
    if (units <= 0) {
        return makeError(ErrorCode::InvalidArgument, "malformed UTF-8: " + lastErrorMessage());
    }
    std::wstring wide(static_cast<std::size_t>(units), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, wide.data(), units) != units) {
        return makeError(ErrorCode::InvalidArgument, "malformed UTF-8: " + lastErrorMessage());
    }
    return wide;
}

std::string narrow(std::wstring_view utf16) {
    if (utf16.empty() || utf16.size() > static_cast<std::size_t>(INT_MAX)) {
        return {};
    }
    const int length = static_cast<int>(utf16.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), length, utf8.data(), bytes, nullptr, nullptr);
    return utf8;
}

std::string errorMessage(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
        MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text;
    if (length == 0 || buffer == nullptr) {
        // No English message table installed: the user's language.
        const DWORD fallback = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
            0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
        if (fallback != 0 && buffer != nullptr) {
            text = narrow(std::wstring_view(buffer, fallback));
        }
    } else {
        text = narrow(std::wstring_view(buffer, length));
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.')) {
        text.pop_back();
    }
    if (text.empty()) {
        text = "unknown error";
    }
    return fmt::format("{} (error {})", text, error);
}

std::string lastErrorMessage() {
    return errorMessage(GetLastError());
}

} // namespace rv::platform::win32
