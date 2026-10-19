#include <rv/ui/ui_system.hpp>

#include <atomic>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Log.h>
#include <SDL3/SDL_timer.h>
#include <spdlog/spdlog.h>

#include <RmlUi_Platform_SDL.h>
#include <RmlUi_Renderer_GL3.h>

#include <rv/core/main_thread.hpp>
#include <rv/core/text.hpp>
#include <rv/platform/graphics_window.hpp>

namespace rv::ui {
namespace {

/// One UiSystem at a time: RmlUi's state is global.
std::atomic<bool> g_uiSystemAlive{false};

/// The file `utf8` names under `root`: a relative path whose lexical normal form stays inside the root.
std::optional<std::filesystem::path> resolveUnder(const std::filesystem::path& root, std::string_view utf8) {
    if (utf8.empty()) {
        return std::nullopt;
    }
    const std::filesystem::path relative = core::pathFromUtf8(utf8).lexically_normal();
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        return std::nullopt;
    }
    for (const std::filesystem::path& part : relative) {
        if (part == "..") {
            return std::nullopt;
        }
    }
    return root / relative;
}

[[nodiscard]] std::FILE* toFile(Rml::FileHandle handle) noexcept {
    return reinterpret_cast<std::FILE*>(handle);
}

/// RmlUi's documents, style sheets, fonts and images, read from the asset root with UTF-8 paths.
class AssetFileInterface final : public Rml::FileInterface {
public:
    explicit AssetFileInterface(std::filesystem::path root) : m_root(std::move(root)) {}

    Rml::FileHandle Open(const Rml::String& path) override {
        const std::optional<std::filesystem::path> resolved = resolveUnder(m_root, path);
        if (!resolved) {
            spdlog::warn("UI file '{}' refused: only paths inside the asset root are read", path);
            return 0;
        }
#if defined(_WIN32)
        std::FILE* file = _wfopen(resolved->c_str(), L"rb");
#else
        std::FILE* file = std::fopen(resolved->c_str(), "rb");
#endif
        return reinterpret_cast<Rml::FileHandle>(file);
    }

    void Close(Rml::FileHandle file) override {
        if (file != 0) {
            std::fclose(toFile(file));
        }
    }

    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
        return std::fread(buffer, 1, size, toFile(file));
    }

    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        return std::fseek(toFile(file), offset, origin) == 0;
    }

    size_t Tell(Rml::FileHandle file) override {
        const long position = std::ftell(toFile(file));
        return position < 0 ? 0 : static_cast<size_t>(position);
    }

private:
    std::filesystem::path m_root;
};

/// The SDL3 backend's system interface (cursors, clipboard, text input) with the application's clock and RmlUi's log
/// routed to spdlog.
class SystemInterface final : public SystemInterface_SDL {
public:
    explicit SystemInterface(SDL_Window* window) : SystemInterface_SDL(window), m_startNs(SDL_GetTicksNS()) {}

    double GetElapsedTime() override {
        if (m_time) {
            return *m_time;
        }
        return static_cast<double>(SDL_GetTicksNS() - m_startNs) * 1.0e-9;
    }

    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        switch (type) {
        case Rml::Log::LT_ERROR:
        case Rml::Log::LT_ASSERT:
            ++m_counts.errors;
            spdlog::error("RmlUi: {}", message);
            break;
        case Rml::Log::LT_WARNING:
            ++m_counts.warnings;
            spdlog::warn("RmlUi: {}", message);
            break;
        case Rml::Log::LT_INFO:
        case Rml::Log::LT_ALWAYS:
            spdlog::info("RmlUi: {}", message);
            break;
        default:
            spdlog::debug("RmlUi: {}", message);
            break;
        }
        return true; // never break into a debugger
    }

    void setTime(double seconds) noexcept { m_time = seconds; }
    [[nodiscard]] double time() { return GetElapsedTime(); }
    [[nodiscard]] UiLogCounts counts() const noexcept { return m_counts; }

private:
    Uint64 m_startNs = 0;
    std::optional<double> m_time;
    UiLogCounts m_counts;
};

/// The backend's OpenGL 3 renderer, counting its draws.
class CountingRenderer final : public RenderInterface_GL3 {
public:
    void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override {
        ++m_draws;
        RenderInterface_GL3::RenderGeometry(handle, translation, texture);
    }
    void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                      Rml::TextureHandle texture) override {
        ++m_draws;
        RenderInterface_GL3::RenderShader(shader, geometry, translation, texture);
    }
    [[nodiscard]] std::uint64_t draws() const noexcept { return m_draws; }

private:
    std::uint64_t m_draws = 0;
};

} // namespace

struct UiSystem::Impl {
    Impl(platform::GraphicsWindow& window, std::filesystem::path root)
        : window(window), assetRoot(root), files(std::move(root)), system(window.handle()) {}
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (initialised) {
            Rml::Shutdown(); // destroys the contexts left, then RmlUi's resources through the renderer
        }
        g_uiSystemAlive.store(false);
    }

    platform::GraphicsWindow& window;
    std::filesystem::path assetRoot;
    AssetFileInterface files;
    SystemInterface system;
    TextInputMethodEditor_SDL textInput;
    /// Created while the OpenGL context is current; destroyed after Rml::Shutdown (members go after the body above).
    std::unique_ptr<CountingRenderer> renderer;
    std::vector<std::string> fonts;
    bool initialised = false;
};

UiSystem::UiSystem(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

UiSystem::~UiSystem() = default;

Expected<std::unique_ptr<UiSystem>> UiSystem::create(platform::GraphicsWindow& window, const UiSystemConfig& config) {
    core::assertMainThread("UiSystem::create");
    bool expected = false;
    if (!g_uiSystemAlive.compare_exchange_strong(expected, true)) {
        return makeError(ErrorCode::InvalidArgument, "another UiSystem is alive (RmlUi's state is global)");
    }
    auto impl = std::make_unique<Impl>(window, config.assetRoot);
    impl->renderer = std::make_unique<CountingRenderer>();
    if (!*impl->renderer) {
        return makeError(ErrorCode::GraphicsError, "the RmlUi OpenGL 3 renderer's shaders did not build (see the log)");
    }
    // No read-back of the OpenGL state around each frame (about 0.6 ms of glGet round trips on NVIDIA's threaded
    // driver): the views leave it at OpenGL's initial values and invalidate the RenderStateCache instead.
    impl->renderer->SetStateBackup(false);
    // The loader is ours (glad, already loaded by the GL device): RmlGL3::Initialize only reports success.
    if (!RmlGL3::Initialize()) {
        return makeError(ErrorCode::GraphicsError, "the RmlUi OpenGL 3 renderer did not initialize");
    }
    Rml::SetSystemInterface(&impl->system);
    Rml::SetFileInterface(&impl->files);
    Rml::SetRenderInterface(impl->renderer.get());
    Rml::SetTextInputHandler(&impl->textInput);
    if (!Rml::Initialise()) {
        return makeError(ErrorCode::PlatformError, "Rml::Initialise failed (see the log)");
    }
    impl->initialised = true;
    for (const std::string& face : config.fontFaces) {
        if (Rml::LoadFontFace(face)) {
            impl->fonts.push_back(face);
        } else {
            spdlog::error("UI font face {} could not be loaded", face);
        }
    }
    if (impl->fonts.empty()) {
        return makeError(ErrorCode::IoError, "no UI font face could be loaded", core::pathToUtf8(config.assetRoot));
    }
    spdlog::info("RmlUi {} initialized: {} font face(s), documents from {}", Rml::GetVersion(), impl->fonts.size(),
                 core::pathToGenericUtf8(config.assetRoot));
    return std::unique_ptr<UiSystem>(new UiSystem(std::move(impl)));
}

void UiSystem::setTime(double seconds) noexcept {
    m_impl->system.setTime(seconds);
}

double UiSystem::time() const noexcept {
    return m_impl->system.time();
}

UiLogCounts UiSystem::logCounts() const noexcept {
    return m_impl->system.counts();
}

const std::filesystem::path& UiSystem::assetRoot() const noexcept {
    return m_impl->assetRoot;
}

platform::GraphicsWindow& UiSystem::window() const noexcept {
    return m_impl->window;
}

const std::vector<std::string>& UiSystem::loadedFontFaces() const noexcept {
    return m_impl->fonts;
}

RenderInterface_GL3& UiSystem::renderer() noexcept {
    return *m_impl->renderer;
}

std::uint64_t UiSystem::drawCalls() const noexcept {
    return m_impl->renderer->draws();
}

void UiSystem::setTexture(std::string_view key, unsigned texture, glm::ivec2 size) {
    m_impl->renderer->SetExternalTexture(Rml::String(key), texture, Rml::Vector2i(size.x, size.y));
}

unsigned UiSystem::texture(std::string_view key) const {
    return m_impl->renderer->ExternalTexture(Rml::String(key));
}

TextInputMethodEditor_SDL& UiSystem::textInputEditor() noexcept {
    return m_impl->textInput;
}

} // namespace rv::ui
