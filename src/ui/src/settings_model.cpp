#include <rv/ui/settings_model.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

#include <fmt/format.h>

namespace rv::ui {
namespace {

using Json = nlohmann::json;

/// A share of 0 to 1 is shown as a percentage (the volumes, the vignette).
[[nodiscard]] bool isShare(const SettingsRow& row) noexcept {
    return row.max <= 1.0;
}

/// Steps add up without the binary noise a file would keep ("0.35000000000000003").
[[nodiscard]] double tidy(double value) noexcept {
    return std::round(value * 10000.0) / 10000.0;
}

} // namespace

std::string settingsValueText(const SettingsRow& row) {
    if (!row.value.is_number()) {
        return {};
    }
    const double value = row.value.get<double>();
    return fmt::format("{:g}", isShare(row) ? std::round(value * 100.0) : value);
}

std::string settingsFill(const SettingsRow& row) {
    if (!row.value.is_number() || !(row.max > row.min)) {
        return "0%";
    }
    const double share = std::clamp((row.value.get<double>() - row.min) / (row.max - row.min), 0.0, 1.0);
    return fmt::format("{:.0f}%", share * 100.0);
}

std::string settingsValueKey(const SettingsRow& row) {
    if (row.value.is_boolean()) {
        return row.value.get<bool>() ? "on" : "off";
    }
    if (row.value.is_string()) {
        return row.value.get<std::string>();
    }
    if (row.value.is_number()) {
        return fmt::format("{:g}", row.value.get<double>());
    }
    return {};
}

std::optional<Json> settingsStep(const SettingsRow& row, int direction, bool wrap) {
    switch (row.kind) {
    case SettingsRowKind::Toggle:
        return Json(!(row.value.is_boolean() && row.value.get<bool>()));
    case SettingsRowKind::Choice: {
        if (row.choices.empty()) {
            return std::nullopt;
        }
        const auto count = static_cast<std::ptrdiff_t>(row.choices.size());
        const auto found = std::find(row.choices.begin(), row.choices.end(), row.value);
        if (found == row.choices.end()) {
            return std::optional<Json>(std::in_place, row.choices.front()); // a hand-written value the row does not list
        }
        std::ptrdiff_t next = (found - row.choices.begin()) + direction;
        if (wrap) {
            next = (next + count) % count;
        } else if (next < 0 || next >= count) {
            return std::nullopt;
        }
        return std::optional<Json>(std::in_place, row.choices[static_cast<std::size_t>(next)]);
    }
    case SettingsRowKind::Slider: {
        if (!row.value.is_number()) {
            if (direction > 0) {
                return Json(row.min); // from the scene's up to the range
            }
            return std::nullopt;
        }
        const double now = row.value.get<double>();
        const double steps = std::round((now - row.min) / row.step);
        const double next = tidy(row.min + (steps + direction) * row.step);
        if (next < row.min) {
            if (row.nullable) {
                return Json(nullptr);
            }
            return now > row.min ? std::optional<Json>(Json(row.min)) : std::nullopt;
        }
        if (next > row.max) {
            return now < row.max ? std::optional<Json>(Json(row.max)) : std::nullopt;
        }
        return Json(next);
    }
    }
    return std::nullopt;
}

std::string_view toString(SettingsCommand::Kind kind) noexcept {
    switch (kind) {
    case SettingsCommand::Kind::Change:
        return "change";
    case SettingsCommand::Kind::Restore:
        return "restore";
    case SettingsCommand::Kind::RestoreAll:
        return "restore-all";
    case SettingsCommand::Kind::Close:
        return "close";
    }
    return "unknown";
}

std::vector<std::string> SettingsModel::groups() const {
    std::vector<std::string> held;
    for (const std::string_view group : settings::kGroups) {
        if (std::any_of(m_rows.begin(), m_rows.end(), [&](const SettingsRow& row) { return row.group == group; })) {
            held.emplace_back(group);
        }
    }
    return held;
}

std::vector<const SettingsRow*> SettingsModel::shown() const {
    std::vector<const SettingsRow*> rows;
    for (const SettingsRow& row : m_rows) {
        if (row.group == m_group) {
            rows.push_back(&row);
        }
    }
    return rows;
}

const SettingsRow* SettingsModel::focusedRow() const {
    const std::vector<const SettingsRow*> rows = shown();
    return m_focus < rows.size() ? rows[m_focus] : nullptr;
}

std::string SettingsModel::rowElement(std::size_t position) {
    return fmt::format("st-row-{}", position);
}

std::string SettingsModel::focusedElement() const {
    return m_focus < shown().size() ? rowElement(m_focus) : std::string(settings::kRestoreAllId);
}

void SettingsModel::open() {
    m_open = true;
    m_confirmAll = false;
    m_focus = 0;
    const std::vector<std::string> held = groups();
    m_group = held.empty() ? std::string(settings::kGroups.front()) : held.front();
    changed();
}

void SettingsModel::close() noexcept {
    if (m_open) {
        m_open = false;
        changed();
    }
}

void SettingsModel::setRows(std::vector<SettingsRow> rows) {
    const SettingsRow* focused = focusedRow();
    const std::string key = focused != nullptr ? focused->key : std::string();
    if (rows == m_rows) {
        return;
    }
    m_rows = std::move(rows);
    const std::vector<std::string> held = groups();
    if (!held.empty() && std::find(held.begin(), held.end(), m_group) == held.end()) {
        m_group = held.front();
        m_focus = 0;
    }
    const std::vector<const SettingsRow*> now = shown();
    if (!key.empty()) {
        for (std::size_t position = 0; position < now.size(); ++position) {
            if (now[position]->key == key) {
                m_focus = position;
            }
        }
    }
    m_focus = std::min(m_focus, now.size());
    changed();
}

void SettingsModel::setDevice(bool pad) noexcept {
    if (m_pad != pad) {
        m_pad = pad;
        changed();
    }
}

void SettingsModel::setGroup(std::string group) {
    if (group != m_group) {
        m_group = std::move(group);
        m_focus = 0;
        m_confirmAll = false;
        changed();
    }
}

void SettingsModel::moveGroup(int direction) {
    const std::vector<std::string> held = groups();
    const auto at = std::find(held.begin(), held.end(), m_group);
    if (at == held.end()) {
        return;
    }
    const auto count = static_cast<std::ptrdiff_t>(held.size());
    const std::ptrdiff_t next = ((at - held.begin()) + direction + count) % count;
    setGroup(held[static_cast<std::size_t>(next)]);
}

void SettingsModel::moveFocus(int direction) {
    const std::size_t last = shown().size();
    if (direction < 0 && m_focus > 0) {
        --m_focus;
    } else if (direction > 0 && m_focus < last) {
        ++m_focus;
    } else {
        return;
    }
    m_confirmAll = false;
    changed();
}

std::optional<SettingsCommand> SettingsModel::stepFocused(int direction, bool wrap) {
    const SettingsRow* row = focusedRow();
    if (row == nullptr) {
        return std::nullopt;
    }
    std::optional<Json> next = settingsStep(*row, direction, wrap);
    if (!next) {
        return std::nullopt;
    }
    return SettingsCommand{SettingsCommand::Kind::Change, row->key, std::move(*next)};
}

std::optional<SettingsCommand> SettingsModel::restoreFocused() {
    const SettingsRow* row = focusedRow();
    if (row == nullptr || !row->overridden) {
        return std::nullopt;
    }
    return SettingsCommand{SettingsCommand::Kind::Restore, row->key, nullptr};
}

std::optional<SettingsCommand> SettingsModel::pressRestoreAll() {
    if (!m_confirmAll) {
        m_confirmAll = true;
        changed();
        return std::nullopt;
    }
    m_confirmAll = false;
    changed();
    return SettingsCommand{SettingsCommand::Kind::RestoreAll, {}, nullptr};
}

std::optional<SettingsCommand> SettingsModel::apply(UiCommand command) {
    if (!m_open) {
        return std::nullopt;
    }
    switch (command) {
    case UiCommand::Up:
        moveFocus(-1);
        return std::nullopt;
    case UiCommand::Down:
        moveFocus(1);
        return std::nullopt;
    case UiCommand::Left:
        return stepFocused(-1, false);
    case UiCommand::Right:
        return stepFocused(1, false);
    case UiCommand::Activate:
        if (focusedRow() == nullptr) {
            return pressRestoreAll();
        }
        return stepFocused(1, true);
    case UiCommand::Info:
        return restoreFocused();
    case UiCommand::FilterPrevious:
        moveGroup(-1);
        return std::nullopt;
    case UiCommand::FilterNext:
        moveGroup(1);
        return std::nullopt;
    case UiCommand::Back:
        if (m_confirmAll) {
            m_confirmAll = false;
            changed();
            return std::nullopt;
        }
        close();
        return SettingsCommand{SettingsCommand::Kind::Close, {}, nullptr};
    default:
        return std::nullopt;
    }
}

std::optional<SettingsCommand> SettingsModel::click(const SettingsClick& click) {
    if (!m_open) {
        return std::nullopt;
    }
    if (click.kind == SettingsClick::Kind::Tab) {
        const std::vector<std::string> held = groups();
        if (click.index < settings::kGroups.size()) {
            const std::string group(settings::kGroups[click.index]);
            if (std::find(held.begin(), held.end(), group) != held.end()) {
                setGroup(group);
            }
        }
        return std::nullopt;
    }
    if (click.kind == SettingsClick::Kind::RestoreAll) {
        const std::size_t last = shown().size();
        if (m_focus != last) {
            m_focus = last;
            changed();
        }
        return pressRestoreAll();
    }
    if (click.index >= shown().size()) {
        return std::nullopt;
    }
    if (m_focus != click.index) {
        m_focus = click.index;
        m_confirmAll = false;
        changed();
    }
    switch (click.kind) {
    case SettingsClick::Kind::Row:
        return focusedRow()->kind == SettingsRowKind::Slider ? std::nullopt : stepFocused(1, true);
    case SettingsClick::Kind::Step:
        return stepFocused(click.direction, false);
    case SettingsClick::Kind::Restore:
        return restoreFocused();
    default:
        return std::nullopt;
    }
}

} // namespace rv::ui
