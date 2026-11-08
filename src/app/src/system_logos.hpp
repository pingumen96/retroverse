#pragma once

// The systems' logos for the product UI (I11 step 3, ADR-0019 §3): each system's logo file (catalog::IdentityLogo, from
// ES-DE's themes or identity.json) rasterized on the worker pool (assets::loadLogo: an SVG at the box's size, a PNG or
// JPEG fitted into it), uploaded on the main thread at most one per frame, and registered with RmlUi as the texture
// "rv-texture:logo-<system>" (ProductUi::setUiTexture). No cache: about 45 logos of at most 512 x 128 pixels take a few
// milliseconds each on the pool (docs/i11/step-2.md). A logo that cannot be read is logged once and left out: the UI
// shows the system's name alone.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rv/app/app_catalog.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/render/texture.hpp>

namespace rv::app {

class ProductUi;

/// The box a logo is fitted into, in pixels: wide, as theme logos are.
inline constexpr int kLogoBoxWidth = 512;
inline constexpr int kLogoBoxHeight = 128;

class SystemLogos {
public:
    explicit SystemLogos(core::WorkerPool& pool);
    ~SystemLogos();
    SystemLogos(const SystemLogos&) = delete;
    SystemLogos& operator=(const SystemLogos&) = delete;

    /// The UI texture key of `system`'s logo ("logo-snes"), and its source for an RML <img> ("rv-texture:logo-snes").
    [[nodiscard]] static std::string key(std::string_view system);
    [[nodiscard]] static std::string source(std::string_view system);

    /// Once per frame: the logos `wanted` (the catalog's systems) asked of the pool when new or changed, at most one
    /// finished logo uploaded and registered with `product`'s UI; a system no longer wanted (or whose file changed)
    /// has its texture unregistered and released.
    void update(const std::vector<SystemLogoFile>& wanted, ProductUi& product);
    /// Every texture unregistered from `product` and released (before the product UI goes).
    void release(ProductUi& product);

    /// `system`'s logo is registered: its <img> can show.
    [[nodiscard]] bool has(std::string_view system) const;
    /// Advances each time a logo is registered or removed (a cache key for the UI's data).
    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }
    [[nodiscard]] std::size_t loaded() const noexcept { return m_textures.size(); }
    [[nodiscard]] std::size_t failed() const noexcept { return m_failed; }

private:
    struct Shared;

    core::WorkerPool& m_pool;
    std::shared_ptr<Shared> m_shared;
    core::JobGeneration m_generation;
    /// The file each system was asked for (asked or loaded), and its texture once registered.
    std::map<std::string, std::filesystem::path, std::less<>> m_asked;
    std::map<std::string, gl::Texture2D, std::less<>> m_textures;
    std::uint64_t m_revision = 0;
    std::size_t m_failed = 0;
};

} // namespace rv::app
