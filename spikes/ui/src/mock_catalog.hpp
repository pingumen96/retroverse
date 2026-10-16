#pragma once

#include <vector>

#include <rv/ui/quick_catalog_model.hpp>

namespace rv::spike_ui {

/// The 20 games of the mock quick catalog: invented titles, studios and descriptions in Italian (no trademark, style
/// §17.2), five generic platform families as the filter chips. Ids "g01" to "g20" in catalog order.
[[nodiscard]] std::vector<ui::CatalogEntry> mockCatalog();

} // namespace rv::spike_ui
