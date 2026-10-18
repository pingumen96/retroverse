#include <rv/ui/filter_count_log.hpp>

#include <utility>

#include <rv/ui/quick_catalog_model.hpp>

namespace rv::ui {

void FilterCountLog::chosen(std::string key) {
    m_key = std::move(key);
}

std::optional<std::string> FilterCountLog::take(const QuickCatalogModel& model) {
    if (!m_key || model.pending()) {
        return std::nullopt;
    }
    const std::string name = m_key->empty() ? std::string("(every game)") : *m_key;
    m_key.reset();
    return "filter '" + name + "', " + std::to_string(model.count()) + " game(s)";
}

} // namespace rv::ui
