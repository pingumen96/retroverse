#include "xr_game.hpp"

#include <optional>
#include <span>
#include <utility>

#include <spdlog/spdlog.h>

#include "screen_service.hpp"

namespace rv::app {

namespace {

/// The window's picture is read back this often for its black bars (I10 step 11).
constexpr std::chrono::milliseconds kProbeInterval{250};

} // namespace

XrGame::~XrGame() {
    // The interop holds the capture's texture: it goes first.
    m_probe.reset();
    m_interop.reset();
    m_capture.reset();
}

void XrGame::stop(ScreenService* screen) {
    // Whether the last frame showed it or not: a hero screen that went while the game ran left the picture set.
    if (screen != nullptr) {
        screen->clearGameWindow();
    }
    m_probe.reset();
    m_bounds.reset();
    m_client = glm::ivec4(0);
    m_crop = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
    m_interop.reset();
    m_capture.reset();
    m_showing = false;
}

void XrGame::probeBars(gl::RenderStateCache& state, XrRecord* record) {
    if (m_probeFailed || !m_interop) {
        return;
    }
    if (!m_probe) {
        auto created = gl::PictureProbe::create("XrGameWindowProbe");
        if (!created) {
            spdlog::warn("VR: the game's window keeps its black bars on the CRT: {}", created.error().toString());
            m_probeFailed = true;
            return;
        }
        m_probe = std::move(*created);
    }
    bool changed = false;
    if (const std::optional<std::span<const std::uint8_t>> pixels = m_probe->poll()) {
        if (record != nullptr) {
            ++record->captureCropSamples;
        }
        changed = m_bounds.update(findPictureBounds(*pixels, gl::PictureProbe::kSize));
    }
    const auto now = std::chrono::steady_clock::now();
    if (!m_probe->pending() && now - m_probedAt >= kProbeInterval) {
        // The client area first (a windowed emulator's frame and title bar), its bars found inside it.
        const glm::ivec4 client = platform_win::clientAreaInCapture(m_window, m_interop->size());
        if (client != m_client) {
            m_client = client;
            m_bounds.reset();
            changed = true;
        }
        if (m_probe->request(m_interop->texture(), m_client, state)) {
            m_probedAt = now;
        }
    }
    if (!changed || m_client.z <= 0 || m_client.w <= 0) {
        return;
    }
    const glm::vec2 size(m_interop->size());
    const glm::vec4 client(static_cast<float>(m_client.x) / size.x, static_cast<float>(m_client.y) / size.y,
                           static_cast<float>(m_client.z) / size.x, static_cast<float>(m_client.w) / size.y);
    const glm::vec4& bars = m_bounds.crop();
    m_crop = glm::vec4(client.x + bars.x * client.z, client.y + bars.y * client.w, bars.z * client.z, bars.w * client.w);
    spdlog::info("VR: the game's window shows on the CRT from ({:.3f}, {:.3f}), {:.3f} x {:.3f} of it (its frame and "
                 "black bars left out)",
                 m_crop.x, m_crop.y, m_crop.z, m_crop.w);
    if (record != nullptr) {
        ++record->captureCropChanges;
    }
}

void XrGame::fail(std::string_view what, const Error& error, ScreenService* screen, XrRecord* record) {
    spdlog::warn("VR: the game's window is not shown on the CRT ({}), the game plays on: {}", what,
                 error.toString());
    if (record != nullptr) {
        record->captureError = std::string(what) + ": " + error.toString();
    }
    m_failed = m_window;
    stop(screen);
}

void XrGame::update(std::uintptr_t window, gl::RenderStateCache& state, ScreenService* screen, XrRecord* record) {
    if (window != m_window) {
        stop(screen);
        m_window = window;
        if (window != 0) {
            spdlog::info("VR: the launched game shows its window, captured onto the CRT");
        }
    }
    if (window == 0 || window == m_failed) {
        return;
    }
    if (!m_capture) {
        auto started = platform_win::WindowCapture::start(window);
        if (!started) {
            fail("the capture did not start", started.error(), screen, record);
            return;
        }
        m_capture = std::move(*started);
        if (record != nullptr) {
            ++record->captureStarts;
        }
    }
    auto polled = m_capture->poll();
    if (!polled) {
        fail("a frame was lost", polled.error(), screen, record);
        return;
    }
    if (!*polled) {
        return; // no new frame: the CRT keeps the last
    }
    if (!m_interop || m_capture->textureGeneration() != m_generation) {
        m_interop.reset();
        auto interop =
            gl::DxInteropTexture::create(m_capture->device(), m_capture->texture(), m_capture->size(), "XrGameWindow");
        if (!interop) {
            fail("OpenGL cannot read the capture", interop.error(), screen, record);
            return;
        }
        m_interop = std::move(*interop);
        m_generation = m_capture->textureGeneration();
    }
    if (Status copied = m_interop->copy(state); !copied) {
        fail("the capture could not be copied", copied.error(), screen, record);
        return;
    }
    probeBars(state, record);
    // No hero screen bound (the lobby, a hall, a service made unbound for the media viewer's clip): captured, shown
    // nowhere, so the UI's panel stays and the eyes keep their scale.
    m_showing = screen != nullptr && screen->showGameWindow(m_interop->texture(), m_interop->size(), m_crop);
    if (record != nullptr) {
        ++record->captureFrames;
        record->captureShownFrames += m_showing ? 1 : 0;
        record->captureSize = m_interop->size();
        record->captureCrop = m_crop;
    }
}

} // namespace rv::app
