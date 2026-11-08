#include <rv/platform_win/audio_endpoint.hpp>

#include <windows.h>

#include <mmdeviceapi.h>
#include <objbase.h>

#include <rv/core/utf8.hpp>
#include <rv/platform_win/win32_util.hpp>

namespace rv::platform_win {

namespace {

/// Releases a COM interface at the end of its scope.
template <class Interface>
struct Released {
    Interface* pointer = nullptr;
    Released() = default;
    Released(const Released&) = delete;
    Released& operator=(const Released&) = delete;
    ~Released() {
        if (pointer != nullptr) {
            pointer->Release();
        }
    }
};

} // namespace

Expected<std::string> defaultAudioOutput() {
    const ComScope com(COINIT_MULTITHREADED);
    if (!com.usable()) {
        return makeError(ErrorCode::Unavailable, "COM cannot be used on this thread", hresultText(com.result()));
    }
    Released<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&enumerator.pointer));
    if (FAILED(result)) {
        return makeError(ErrorCode::Unavailable, "the audio devices cannot be listed", hresultText(result));
    }
    Released<IMMDevice> device;
    result = enumerator.pointer->GetDefaultAudioEndpoint(eRender, eConsole, &device.pointer);
    if (result == static_cast<HRESULT>(0x80070490) /* E_NOTFOUND */) {
        return makeError(ErrorCode::NotFound, "the system has no audio output", hresultText(result));
    }
    if (FAILED(result)) {
        return makeError(ErrorCode::Unavailable, "the default audio output cannot be read", hresultText(result));
    }
    LPWSTR id = nullptr;
    result = device.pointer->GetId(&id);
    if (FAILED(result) || id == nullptr) {
        return makeError(ErrorCode::Unavailable, "the default audio output has no id", hresultText(result));
    }
    std::string text = core::wideToUtf8Lossy(id);
    CoTaskMemFree(id);
    return text;
}

} // namespace rv::platform_win
