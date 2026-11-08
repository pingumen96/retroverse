#include "system_logos.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/assets/svg_raster.hpp>
#include <rv/core/utf8.hpp>

#include "product_ui.hpp"

namespace rv::app {

namespace {

/// Behind the covers and the room's work: the UI can wait for its logos.
constexpr std::int64_t kJobPriority = -10;

/// RmlUi composites premultiplied alpha (RmlUi_Renderer_GL3): loadLogo()'s straight alpha multiplied in, in the sRGB
/// values the UI blends.
void premultiply(assets::ImageData& image) {
    for (std::size_t at = 0; at + 3 < image.pixels.size(); at += 4) {
        const unsigned alpha = image.pixels[at + 3];
        for (std::size_t channel = 0; channel < 3; ++channel) {
            image.pixels[at + channel] = static_cast<std::uint8_t>((image.pixels[at + channel] * alpha + 127u) / 255u);
        }
    }
}

} // namespace

struct SystemLogos::Shared {
    struct Finished {
        std::string system;
        std::filesystem::path file;
        std::optional<assets::ImageData> picture;
        std::string error;
    };
    std::mutex mutex;
    std::vector<Finished> finished;
};

SystemLogos::SystemLogos(core::WorkerPool& pool) : m_pool(pool), m_shared(std::make_shared<Shared>()) {}

SystemLogos::~SystemLogos() {
    m_pool.cancel(m_generation);
}

std::string SystemLogos::key(std::string_view system) {
    return "logo-" + std::string(system);
}

std::string SystemLogos::source(std::string_view system) {
    return "rv-texture:" + key(system);
}

bool SystemLogos::has(std::string_view system) const {
    return m_textures.find(system) != m_textures.end();
}

void SystemLogos::update(const std::vector<SystemLogoFile>& wanted, ProductUi& product) {
    // Systems gone or whose file changed: unregistered, forgotten (their pictures dropped when they come).
    for (auto asked = m_asked.begin(); asked != m_asked.end();) {
        const auto still = std::find_if(wanted.begin(), wanted.end(), [&](const SystemLogoFile& logo) {
            return logo.system == asked->first && logo.file == asked->second;
        });
        if (still != wanted.end()) {
            ++asked;
            continue;
        }
        if (const auto texture = m_textures.find(asked->first); texture != m_textures.end()) {
            product.setUiTexture(key(asked->first), 0, {});
            m_textures.erase(texture);
            ++m_revision;
        }
        asked = m_asked.erase(asked);
    }
    // New ones, asked of the pool.
    for (const SystemLogoFile& logo : wanted) {
        if (logo.file.empty() || m_asked.contains(logo.system)) {
            continue;
        }
        m_asked.emplace(logo.system, logo.file);
        core::JobOptions options;
        options.priority = kJobPriority;
        options.generation = &m_generation;
        const Status submitted = m_pool.submit(
            [shared = m_shared, system = logo.system, file = logo.file](const core::JobContext& job) {
                if (job.cancelled()) {
                    return;
                }
                Shared::Finished finished{system, file, std::nullopt, {}};
                if (auto picture = assets::loadLogo(file, kLogoBoxWidth, kLogoBoxHeight)) {
                    premultiply(*picture);
                    finished.picture = std::move(*picture);
                } else {
                    finished.error = picture.error().toString();
                }
                const std::lock_guard lock(shared->mutex);
                shared->finished.push_back(std::move(finished));
            },
            options);
        if (!submitted) {
            spdlog::warn("The logo of {} cannot be asked for: {}", logo.system, submitted.error().toString());
        }
    }
    // One finished logo a frame: its upload is a few hundred microseconds.
    std::optional<Shared::Finished> next;
    {
        const std::lock_guard lock(m_shared->mutex);
        if (!m_shared->finished.empty()) {
            next = std::move(m_shared->finished.front());
            m_shared->finished.erase(m_shared->finished.begin());
        }
    }
    if (!next) {
        return;
    }
    const auto asked = m_asked.find(next->system);
    if (asked == m_asked.end() || asked->second != next->file) {
        return; // forgotten meanwhile
    }
    if (!next->picture) {
        ++m_failed;
        spdlog::warn("The logo of {} ({}) is left out: {}", next->system, core::pathToUtf8Lossy(next->file),
                     next->error);
        return;
    }
    gl::TextureDesc desc;
    // Its values as they are (the UI blends sRGB-encoded values), with mips for the panels' smaller sizes.
    desc.colorSpace = gl::ColorSpace::Linear;
    desc.mipmaps = true;
    desc.sampling.wrap = gl::TextureWrap::ClampToEdge;
    desc.sampling.filter = gl::TextureFilter::Trilinear;
    desc.memoryKind = gl::GpuMemoryKind::UiTextures;
    auto texture = gl::Texture2D::create(*next->picture, desc, fmt::format("Logo {}", next->system));
    if (!texture) {
        ++m_failed;
        spdlog::warn("The logo of {} cannot be uploaded: {}", next->system, texture.error().toString());
        return;
    }
    product.setUiTexture(key(next->system), texture->id(), texture->size());
    m_textures.insert_or_assign(next->system, std::move(*texture));
    ++m_revision;
}

void SystemLogos::release(ProductUi& product) {
    m_pool.cancel(m_generation);
    for (const auto& [system, texture] : m_textures) {
        product.setUiTexture(key(system), 0, {});
    }
    if (!m_textures.empty()) {
        ++m_revision;
    }
    m_textures.clear();
    m_asked.clear();
}

} // namespace rv::app
