#include <rv/platform_win/window_capture.hpp>

// Windows.Graphics.Capture through C++/WinRT (the Windows SDK's cppwinrt headers), D3D11 for its frames. The WinRT
// calls throw winrt::hresult_error: every one is caught here, none leaves this file.
#include <windows.h>
#include <unknwn.h>

#include <d3d11.h>
#include <dxgi.h>

#pragma warning(push, 0)
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#pragma warning(pop)

#include <atomic>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

namespace rv::platform_win {
namespace {

namespace capture = winrt::Windows::Graphics::Capture;
namespace directx = winrt::Windows::Graphics::DirectX;

Error captureError(std::string_view what, HRESULT result) {
    return Error{ErrorCode::PlatformError, fmt::format("{} failed: HRESULT 0x{:08X}", what, static_cast<unsigned>(result)),
                 "window capture"};
}

Error captureError(std::string_view what, const winrt::hresult_error& error) {
    return Error{ErrorCode::PlatformError,
                 fmt::format("{} failed: HRESULT 0x{:08X} ({})", what, static_cast<unsigned>(error.code().value),
                             winrt::to_string(error.message())),
                 "window capture"};
}

} // namespace

struct WindowCapture::Impl {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    directx::Direct3D11::IDirect3DDevice winrtDevice{nullptr};
    capture::GraphicsCaptureItem item{nullptr};
    capture::Direct3D11CaptureFramePool pool{nullptr};
    capture::GraphicsCaptureSession session{nullptr};
    winrt::event_token closedToken{};
    winrt::Windows::Graphics::SizeInt32 poolSize{};
    winrt::com_ptr<ID3D11Texture2D> texture;
    glm::ivec2 size{0};
    std::uint64_t generation = 0;
    std::uint64_t frames = 0;
    std::atomic<bool> closed{false};

    ~Impl() {
        try {
            if (item) {
                item.Closed(closedToken);
            }
            if (session) {
                session.Close();
            }
            if (pool) {
                pool.Close();
            }
        } catch (...) {
            // Closing never throws on to the caller.
        }
    }
};

bool WindowCapture::supported() noexcept {
    try {
        return capture::GraphicsCaptureSession::IsSupported();
    } catch (...) {
        return false;
    }
}

WindowCapture::WindowCapture(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

WindowCapture::~WindowCapture() = default;

Expected<std::unique_ptr<WindowCapture>> WindowCapture::start(std::uintptr_t window) {
    const HWND hwnd = reinterpret_cast<HWND>(window);
    if (hwnd == nullptr || IsWindow(hwnd) == FALSE) {
        return makeError(ErrorCode::Unsupported, "no such window", "window capture");
    }
    if (!supported()) {
        return makeError(ErrorCode::Unsupported, "this system cannot capture windows (Windows 10 1903 or later)",
                         "window capture");
    }
    auto impl = std::make_unique<Impl>();
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (const HRESULT created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                                  impl->device.put(), nullptr, impl->context.put());
        FAILED(created)) {
        return tl::unexpected(captureError("D3D11CreateDevice", created));
    }
    try {
        const winrt::com_ptr<IDXGIDevice> dxgi = impl->device.as<IDXGIDevice>();
        winrt::com_ptr<IInspectable> inspectable;
        if (const HRESULT wrapped = CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put());
            FAILED(wrapped)) {
            return tl::unexpected(captureError("CreateDirect3D11DeviceFromDXGIDevice", wrapped));
        }
        impl->winrtDevice = inspectable.as<directx::Direct3D11::IDirect3DDevice>();
        const auto interop = winrt::get_activation_factory<capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (const HRESULT made = interop->CreateForWindow(hwnd, winrt::guid_of<capture::GraphicsCaptureItem>(),
                                                          winrt::put_abi(impl->item));
            FAILED(made)) {
            return tl::unexpected(captureError("IGraphicsCaptureItemInterop::CreateForWindow", made));
        }
        impl->poolSize = impl->item.Size();
        impl->pool = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            impl->winrtDevice, directx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, impl->poolSize);
        impl->session = impl->pool.CreateCaptureSession(impl->item);
        Impl* raw = impl.get();
        impl->closedToken = impl->item.Closed([raw](const auto&, const auto&) { raw->closed = true; });
        using winrt::Windows::Foundation::Metadata::ApiInformation;
        if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession",
                                              L"IsCursorCaptureEnabled")) {
            impl->session.IsCursorCaptureEnabled(false);
        }
        if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession",
                                              L"IsBorderRequired")) {
            try {
                impl->session.IsBorderRequired(false);
            } catch (const winrt::hresult_error& error) {
                spdlog::info("Window capture: the yellow border stays ({})", winrt::to_string(error.message()));
            }
        }
        impl->session.StartCapture();
    } catch (const winrt::hresult_error& error) {
        return tl::unexpected(captureError("Windows.Graphics.Capture", error));
    }
    spdlog::info("Window capture: started, {}x{}", impl->poolSize.Width, impl->poolSize.Height);
    return std::unique_ptr<WindowCapture>(new WindowCapture(std::move(impl)));
}

Expected<bool> WindowCapture::poll() {
    Impl& impl = *m_impl;
    try {
        // The newest frame: the older ones the pool kept are let go.
        capture::Direct3D11CaptureFrame frame{nullptr};
        while (const capture::Direct3D11CaptureFrame next = impl.pool.TryGetNextFrame()) {
            frame = next;
        }
        if (!frame) {
            return false;
        }
        const winrt::Windows::Graphics::SizeInt32 content = frame.ContentSize();
        if (content.Width <= 0 || content.Height <= 0) {
            return false;
        }
        if (content.Width != impl.poolSize.Width || content.Height != impl.poolSize.Height) {
            impl.poolSize = content;
            impl.pool.Recreate(impl.winrtDevice, directx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, content);
        }
        const auto access =
            frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> source;
        if (const HRESULT got = access->GetInterface(__uuidof(ID3D11Texture2D), source.put_void()); FAILED(got)) {
            return tl::unexpected(captureError("IDirect3DDxgiInterfaceAccess::GetInterface", got));
        }
        const glm::ivec2 size(content.Width, content.Height);
        if (!impl.texture || impl.size != size) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(size.x);
            desc.Height = static_cast<UINT>(size.y);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            impl.texture = nullptr;
            if (const HRESULT made = impl.device->CreateTexture2D(&desc, nullptr, impl.texture.put()); FAILED(made)) {
                return tl::unexpected(captureError("ID3D11Device::CreateTexture2D", made));
            }
            impl.size = size;
            ++impl.generation;
        }
        const D3D11_BOX box{0, 0, 0, static_cast<UINT>(size.x), static_cast<UINT>(size.y), 1};
        impl.context->CopySubresourceRegion(impl.texture.get(), 0, 0, 0, 0, source.get(), 0, &box);
        impl.context->Flush();
        ++impl.frames;
        return true;
    } catch (const winrt::hresult_error& error) {
        return tl::unexpected(captureError("a captured frame", error));
    }
}

void* WindowCapture::device() const noexcept {
    return m_impl->device.get();
}

void* WindowCapture::texture() const noexcept {
    return m_impl->texture.get();
}

glm::ivec2 WindowCapture::size() const noexcept {
    return m_impl->size;
}

std::uint64_t WindowCapture::textureGeneration() const noexcept {
    return m_impl->generation;
}

bool WindowCapture::closed() const noexcept {
    return m_impl->closed;
}

std::uint64_t WindowCapture::frames() const noexcept {
    return m_impl->frames;
}

} // namespace rv::platform_win
