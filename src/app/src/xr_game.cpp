#include "xr_game.hpp"

#include <utility>

#include <spdlog/spdlog.h>

#include "screen_service.hpp"

namespace rv::app {

XrGame::~XrGame() {
    // The interop holds the capture's texture: it goes first.
    m_interop.reset();
    m_capture.reset();
}

void XrGame::stop(ScreenService* screen) {
    if (m_showing && screen != nullptr) {
        screen->clearGameWindow();
    }
    m_interop.reset();
    m_capture.reset();
    m_showing = false;
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
    if (screen != nullptr) {
        screen->showGameWindow(m_interop->texture(), m_interop->size());
        m_showing = true;
    }
    if (record != nullptr) {
        ++record->captureFrames;
        record->captureSize = m_interop->size();
    }
}

} // namespace rv::app
