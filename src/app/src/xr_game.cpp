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
/// A window whose capture failed is tried again this much later, at most kMaxAttempts times (I11 step 11).
constexpr std::chrono::milliseconds kRetryDelay{2000};
constexpr int kMaxAttempts = 3;
/// A window whose samples stay black this long is reported (ADR-0020 §5): longer than a game's usual black loading.
constexpr std::chrono::milliseconds kBlackReport{5000};

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
    m_large = false;
    m_onCrt = false;
}

unsigned XrGame::texture() const noexcept {
    return m_interop ? m_interop->texture() : 0u;
}

glm::ivec2 XrGame::size() const noexcept {
    return m_interop ? m_interop->size() : glm::ivec2(0);
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
        const auto sampledAt = std::chrono::steady_clock::now();
        if (isBlackPicture(*pixels)) {
            if (!m_blackSince) {
                m_blackSince = sampledAt;
            } else if (!m_blackReported && sampledAt - *m_blackSince >= kBlackReport) {
                m_blackReported = true;
                spdlog::warn("VR: the game's window has shown only black for {} s: if the emulator runs in exclusive "
                             "fullscreen, it cannot be captured (set it to borderless)",
                             std::chrono::duration_cast<std::chrono::seconds>(kBlackReport).count());
                if (record != nullptr) {
                    record->captureBlack = true;
                }
            }
        } else {
            m_blackSince.reset();
        }
        if (record != nullptr) {
            record->captureColor = meanPictureColor(*pixels);
        }
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
    m_failedAt = std::chrono::steady_clock::now();
    ++m_attempts;
    stop(screen);
}

void XrGame::update(std::uintptr_t window, gl::RenderStateCache& state, ScreenService* screen, XrRecord* record,
                    bool largeScreen) {
    if (window != m_window) {
        stop(screen);
        const bool another = m_window != 0 && window != 0;
        m_window = window;
        m_failed = 0;
        m_attempts = 0;
        m_blackSince.reset();
        m_blackReported = false;
        if (window != 0) {
            spdlog::info(another ? "VR: the launched game now shows in another window, captured onto the CRT"
                                 : "VR: the launched game shows its window, captured onto the CRT");
            if (record != nullptr) {
                ++record->captureWindows;
            }
        }
    }
    if (window == 0) {
        return;
    }
    if (window == m_failed) {
        // Tried again a little later, a few times: a window not yet ready, or closed and made again.
        if (m_attempts >= kMaxAttempts || std::chrono::steady_clock::now() - m_failedAt < kRetryDelay) {
            return;
        }
        m_failed = 0;
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
    if (m_capture->closed()) {
        spdlog::info("VR: the game's window closed its capture; the launcher's next window takes the CRT");
        if (record != nullptr) {
            ++record->captureClosed;
        }
        m_failed = m_window;
        m_failedAt = std::chrono::steady_clock::now();
        ++m_attempts;
        stop(screen);
        return;
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
    // No hero screen bound (the lobby, a hall, a service made unbound for the media viewer's clip), or xr.json's choice:
    // the large screen in front of the user shows it (I11 step 14).
    const bool onCrt =
        !largeScreen && screen != nullptr && screen->showGameWindow(m_interop->texture(), m_interop->size(), m_crop);
    if (!onCrt && m_onCrt && screen != nullptr) {
        screen->clearGameWindow();
    }
    if (onCrt != m_onCrt || !m_showing) {
        spdlog::info(onCrt ? "VR: the game shows on the room's CRT" : "VR: the game shows on the large screen");
    }
    m_onCrt = onCrt;
    m_large = !onCrt;
    m_showing = true;
    if (record != nullptr) {
        ++record->captureFrames;
        record->captureShownFrames += 1;
        record->largeScreenFrames += m_large ? 1 : 0;
        record->captureSize = m_interop->size();
        record->captureCrop = m_crop;
    }
}

} // namespace rv::app
