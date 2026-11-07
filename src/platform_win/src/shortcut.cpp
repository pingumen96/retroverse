// Win32SystemProbe::readShortcut (I11 step 13, ADR-0020 §3): a .lnk file read through IShellLinkW, never resolved and
// never saved.

#include <rv/platform_win/system_probe.hpp>

#include <cwchar>
#include <string>
#include <system_error>

#include <rv/platform_win/win32_util.hpp>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <rv/core/file_io.hpp>
#include <rv/core/utf8.hpp>

namespace rv::platform_win {
namespace {

using Microsoft::WRL::ComPtr;

/// The longest text read from a shortcut: Windows' longest command line.
constexpr int kMaxShortcutText = 32768;

/// The text a getter wrote into `buffer`, up to its first NUL.
std::wstring textOf(const std::wstring& buffer) {
    return std::wstring(buffer.c_str(), std::wcslen(buffer.c_str()));
}

} // namespace

Expected<core::ShellShortcut> Win32SystemProbe::readShortcut(const std::filesystem::path& path) const {
    const std::string shown = core::pathToUtf8Lossy(path);
    std::error_code ignored;
    if (!path.is_absolute() ||
        !std::filesystem::is_regular_file(std::filesystem::path(core::extendedLengthPath(path)), ignored)) {
        return makeError(ErrorCode::NotFound, "no such shortcut", shown);
    }
    const ComScope com(COINIT_MULTITHREADED);
    if (!com.usable()) {
        return makeError(ErrorCode::PlatformError, "COM cannot be used here (" + hresultText(com.result()) + ")",
                         shown);
    }
    ComPtr<IShellLinkW> link;
    HRESULT result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(result)) {
        return makeError(ErrorCode::PlatformError, "the shell's shortcut object cannot be made (" +
                                                       hresultText(result) + ")",
                         shown);
    }
    ComPtr<IPersistFile> file;
    if (result = link.As(&file); FAILED(result)) {
        return makeError(ErrorCode::PlatformError, "the shortcut object reads no files (" + hresultText(result) + ")",
                         shown);
    }
    // Read only: the shortcut is never resolved (IShellLinkW::Resolve would search for a moved target and could update
    // the file) and never saved.
    if (result = file->Load(path.c_str(), STGM_READ); FAILED(result)) {
        return makeError(ErrorCode::ValidationFailed, "not a shortcut Windows reads (" + hresultText(result) + ")",
                         shown);
    }
    core::ShellShortcut shortcut;
    std::wstring buffer(kMaxShortcutText, L'\0');
    WIN32_FIND_DATAW found{};
    // Without SLGP_RAWPATH the environment variables of the target are expanded, as the shell expands them.
    result = link->GetPath(buffer.data(), kMaxShortcutText, &found, 0);
    const std::wstring target = SUCCEEDED(result) ? textOf(buffer) : std::wstring();
    if (result != S_OK || target.empty()) {
        return makeError(ErrorCode::ValidationFailed,
                         "the shortcut opens no file: a shell folder, a store app or a web address", shown);
    }
    shortcut.target = std::filesystem::path(target);
    if (!shortcut.target.is_absolute()) {
        return makeError(ErrorCode::ValidationFailed,
                         "the shortcut's target is not an absolute path: " + core::pathToUtf8Lossy(shortcut.target),
                         shown);
    }
    std::fill(buffer.begin(), buffer.end(), L'\0');
    if (result = link->GetArguments(buffer.data(), kMaxShortcutText); FAILED(result)) {
        return makeError(ErrorCode::ValidationFailed, "the shortcut's arguments cannot be read (" +
                                                          hresultText(result) + ")",
                         shown);
    }
    auto arguments = core::wideToUtf8(textOf(buffer));
    if (!arguments) {
        return makeError(ErrorCode::ValidationFailed, "the shortcut's arguments are not valid text", shown);
    }
    shortcut.arguments = std::move(*arguments);
    std::fill(buffer.begin(), buffer.end(), L'\0');
    if (SUCCEEDED(link->GetWorkingDirectory(buffer.data(), kMaxShortcutText))) {
        shortcut.workingDirectory = std::filesystem::path(textOf(buffer));
    }
    ComPtr<IShellLinkDataList> data;
    DWORD flags = 0;
    if (SUCCEEDED(link.As(&data)) && SUCCEEDED(data->GetFlags(&flags))) {
        shortcut.runAsAdministrator = (flags & SLDF_RUNAS_USER) != 0;
    }
    return shortcut;
}

} // namespace rv::platform_win
