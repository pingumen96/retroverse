#include <rv/core/system_error.hpp>

#include <windows.h>

#include <fmt/format.h>

#include <rv/core/file_io.hpp>
#include <rv/core/utf8.hpp>

namespace rv::core {

std::string systemErrorMessage(std::uint32_t code) {
    wchar_t* buffer = nullptr;
    constexpr DWORD kFlags =
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD length = FormatMessageW(kFlags, nullptr, code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                                  reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        // No English message table installed: the user's language.
        length = FormatMessageW(kFlags, nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    }
    std::string text;
    if (length != 0 && buffer != nullptr) {
        text = wideToUtf8Lossy(std::wstring_view(buffer, length));
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
    return fmt::format("{} (error {})", text, code);
}

std::string lastSystemErrorMessage() {
    return systemErrorMessage(GetLastError());
}

ErrorCode fileErrorCode(std::uint32_t code) noexcept {
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_DIRECTORY:
        return ErrorCode::NotFound;
    case ERROR_NOT_READY:
    case ERROR_INVALID_DRIVE:
    case ERROR_REM_NOT_LIST:
    case ERROR_BAD_NETPATH:
    case ERROR_DEV_NOT_EXIST:
    case ERROR_UNEXP_NET_ERR:
    case ERROR_NETNAME_DELETED:
    case ERROR_BAD_NET_NAME:
    case ERROR_UNRECOGNIZED_VOLUME:
    case ERROR_NO_MEDIA_IN_DRIVE:
    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_NETWORK_UNREACHABLE:
    case ERROR_HOST_UNREACHABLE:
    case ERROR_NOT_CONNECTED:
    case ERROR_DEVICE_NOT_AVAILABLE:
        return ErrorCode::Unavailable;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return ErrorCode::Conflict;
    default:
        return ErrorCode::IoError;
    }
}

ErrorCode fileErrorCode(std::uint32_t code, const std::filesystem::path& path) {
    const ErrorCode category = fileErrorCode(code);
    if (category != ErrorCode::NotFound) {
        return category;
    }
    // The root of the drive ("X:\") or of the share ("\\server\share\") the path lies on.
    const std::wstring text = extendedLengthPath(path);
    std::wstring root;
    if (text.starts_with(L"\\\\?\\UNC\\")) {
        const std::size_t server = text.find(L'\\', 8);
        const std::size_t share = server == std::wstring::npos ? server : text.find(L'\\', server + 1);
        root = L"\\\\" + text.substr(8, share == std::wstring::npos ? std::wstring::npos : share - 8) + L"\\";
    } else if (text.starts_with(L"\\\\?\\") && text.size() >= 6 && text[5] == L':') {
        root = text.substr(4, 2) + L"\\";
    }
    if (!root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_NO_ROOT_DIR) {
        return ErrorCode::Unavailable;
    }
    return category;
}

} // namespace rv::core
