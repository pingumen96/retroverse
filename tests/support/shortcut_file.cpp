#include "support/shortcut_file.hpp"

#include <string>
#include <thread>

#include <rv/platform_win/win32_util.hpp>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <rv/core/utf8.hpp>

namespace rv::test {

namespace {

Status writeOnThisThread(const std::filesystem::path& file, const std::filesystem::path& target,
                         const std::string& arguments, const std::filesystem::path& workingDirectory,
                         bool administrator) {
    const auto failed = [&](std::string_view what, HRESULT result) {
        return makeError(ErrorCode::PlatformError,
                         std::string(what) + " failed (" + platform_win::hresultText(result) + ")",
                         core::pathToUtf8Lossy(file));
    };
    const platform_win::ComScope com(COINIT_APARTMENTTHREADED);
    if (!com.usable()) {
        return failed("CoInitializeEx", com.result());
    }
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    HRESULT result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(result)) {
        return failed("CoCreateInstance", result);
    }
    const auto wideArguments = core::utf8ToWide(arguments);
    if (!wideArguments) {
        return propagate(wideArguments.error());
    }
    if (result = link->SetPath(target.c_str()); FAILED(result)) {
        return failed("SetPath", result);
    }
    if (result = link->SetArguments(wideArguments->c_str()); FAILED(result)) {
        return failed("SetArguments", result);
    }
    if (!workingDirectory.empty()) {
        if (result = link->SetWorkingDirectory(workingDirectory.c_str()); FAILED(result)) {
            return failed("SetWorkingDirectory", result);
        }
    }
    if (administrator) {
        Microsoft::WRL::ComPtr<IShellLinkDataList> data;
        DWORD flags = 0;
        if (FAILED(link.As(&data)) || FAILED(data->GetFlags(&flags)) ||
            FAILED(data->SetFlags(flags | SLDF_RUNAS_USER))) {
            return failed("SetFlags", E_FAIL);
        }
    }
    Microsoft::WRL::ComPtr<IPersistFile> persist;
    if (result = link.As(&persist); FAILED(result)) {
        return failed("IPersistFile", result);
    }
    if (result = persist->Save(file.c_str(), TRUE); FAILED(result)) {
        return failed("IPersistFile::Save", result);
    }
    return ok();
}

} // namespace

Status writeShortcut(const std::filesystem::path& file, const std::filesystem::path& target,
                     const std::string& arguments, const std::filesystem::path& workingDirectory,
                     bool administrator) {
    // The shell saves shortcuts from a single-threaded apartment: a thread of its own, whatever this one's is.
    Status status = ok();
    std::thread([&] { status = writeOnThisThread(file, target, arguments, workingDirectory, administrator); }).join();
    return status;
}

} // namespace rv::test
