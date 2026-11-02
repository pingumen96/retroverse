#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// The statistics screen's content from what the catalog worker read (ADR-0018 §9): the figures in text, as they show.

#include <rv/app/directory_data.hpp>
#include <rv/catalog/statistics.hpp>
#include <rv/ui/statistics_model.hpp>

namespace rv::app {

/// What the screen shows of `statistics`, its days through `localDate` (machineLocalDate() in the app).
[[nodiscard]] ui::StatisticsContent statisticsContentOf(const catalog::PlayStatistics& statistics,
                                                        const LocalDate& localDate);

} // namespace rv::app
