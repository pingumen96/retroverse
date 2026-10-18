#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.

#include <optional>
#include <string>

namespace rv::ui {

class QuickCatalogModel;

/// When the quick catalog logs how many games a filter lets through: once the list that answers it is the one shown.
/// A source that materializes on its worker (the ES-DE catalog) answers a frame or more after the filter was chosen,
/// and the model keeps the previous list meanwhile (QuickCatalogModel::pending), so the count read when the filter is
/// chosen is the previous list's (as the M1 manual session's log showed). Main thread only.
class FilterCountLog {
public:
    /// A filter (its key; empty: every game) became the active one: its count is logged once its list is shown.
    void chosen(std::string key);
    /// The line to log ("filter '<key>', <N> game(s)"; "(every game)" for the empty key) once the model shows the list
    /// of the latest query, which the latest chosen filter shaped: no query pending. Nothing while one is pending (the
    /// list shown is still an earlier one) or when no filter was chosen since the last line.
    [[nodiscard]] std::optional<std::string> take(const QuickCatalogModel& model);
    /// A filter whose list has not been shown yet.
    [[nodiscard]] bool waiting() const noexcept { return m_key.has_value(); }

private:
    std::optional<std::string> m_key;
};

} // namespace rv::ui
