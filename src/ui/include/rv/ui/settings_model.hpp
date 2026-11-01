#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.
//
// The settings screen (ADR-0018 §3; assets/ui/settings.rml through ui::SettingsScreen): one group of rows at a time, the
// rows given by the application (app::settingRows() with their values). The model only moves the focus and answers with
// what a press asks; the application applies it and gives the rows back (setRows()), so the screen always shows what is in
// force. Every string is the document's: a row is known by its key, a value by its key.
//
// | Command                      | Does                                                                    |
// |------------------------------|-------------------------------------------------------------------------|
// | Up, Down                     | the focused row; the last focus of a group is "Ripristina tutto"         |
// | Left, Right                  | the row's previous or next value                                         |
// | Activate                     | a toggle flips, a choice moves on (from its last back to its first), a   |
// |                              | slider steps up; on "Ripristina tutto" a confirmation, then all of them  |
// | Info                         | "Ripristina": the row's key removed, when settings.json holds it         |
// | FilterPrevious, FilterNext   | the group                                                                |
// | Back                         | the confirmation dropped, else the screen closes                         |
//
// Clicks mirror the keys (ADR-0018 §12: VR's ray is the mouse): a tab, a row (it is focused and takes Activate unless it
// is a slider), its arrows, its Ripristina, "Ripristina tutto".
//
// Remapping (ADR-0018 §7, I7 step 3): Activate or a click on a binding row starts a capture. While it waits no command
// reaches the screen: the application reads the raw input and answers with captured(), or tick() ends it after
// kCaptureSeconds, or a click on Annulla does. A clash offers its swap: Activate takes it, Back drops it; both click too.
// "Ripristina predefiniti" (an action row) asks once, as "Ripristina tutto" does.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/input/binding.hpp>
#include <rv/ui/ui_command.hpp>

namespace rv::ui {

namespace settings {
inline constexpr std::string_view kDocumentPath = "ui/settings.rml";
inline constexpr std::string_view kDataModel = "settings";
inline constexpr std::string_view kPanelId = "settings-panel";
inline constexpr std::string_view kListId = "st-list";
inline constexpr std::string_view kRestoreAllId = "st-restore-all";
inline constexpr std::string_view kTabEvent = "tab";
inline constexpr std::string_view kRowEvent = "row";
inline constexpr std::string_view kStepEvent = "step";
inline constexpr std::string_view kRestoreEvent = "restore";
inline constexpr std::string_view kRestoreAllEvent = "restore_all";
inline constexpr std::string_view kCaptureCancelEvent = "capture_cancel";
inline constexpr std::string_view kCaptureSwapEvent = "capture_swap";
/// How long a capture waits for a press.
inline constexpr double kCaptureSeconds = 5.0;
/// The groups, the tabs' order (app::SettingGroup's names).
inline constexpr std::array<std::string_view, 5> kGroups = {"display", "audio", "world", "controls", "vr"};
} // namespace settings

enum class SettingsRowKind : std::uint8_t {
    Toggle,
    Choice,
    Slider,
    Binding, ///< an action's key and pad button: Activate captures a press
    Action,  ///< a button that asks once: "Ripristina predefiniti"
};

/// A row as the screen shows it. `value` is what is in force.
struct SettingsRow {
    /// The row's key (app::SettingRow::id()): the document's labels and values are written against it.
    std::string key;
    /// One of settings::kGroups.
    std::string group;
    SettingsRowKind kind = SettingsRowKind::Toggle;
    nlohmann::json value;
    /// A choice's values, in order.
    std::vector<nlohmann::json> choices;
    /// A slider's range and step; `nullable`: null lies below min ("della scena").
    double min = 0.0;
    double max = 1.0;
    double step = 0.1;
    bool nullable = false;
    /// settings.json holds it ("Ripristina" shows).
    bool overridden = false;
    /// What the row says about when it applies: "" at once, "commit" (the floor is committed again, within 3 s), "session"
    /// (at the next VR session), "start" (at the next start).
    std::string note;
    /// A binding's key and pad button as named on screen ("" none).
    std::string keyLabel;
    std::string padLabel;
    /// The list a binding row opens: "world", "ui"; "" the rows after the first of a list.
    std::string heading;

    bool operator==(const SettingsRow&) const = default;
};

/// A slider's value as text: a share as a percentage ("75"), else the number ("1.25"); "" for null.
[[nodiscard]] std::string settingsValueText(const SettingsRow& row);
/// A slider's filled part, "0%" to "100%".
[[nodiscard]] std::string settingsFill(const SettingsRow& row);
/// A toggle's or a choice's value as the document's key: "on", "off", a string as it is, a number as text ("1.25").
[[nodiscard]] std::string settingsValueKey(const SettingsRow& row);
/// `row`'s value `direction` (+1, -1) steps away: a toggle flipped, a choice's neighbour (`wrap`: past its ends), a
/// slider snapped to its steps within its range; nothing when it stays.
[[nodiscard]] std::optional<nlohmann::json> settingsStep(const SettingsRow& row, int direction, bool wrap);

/// What the screen asks of the app.
struct SettingsCommand {
    enum class Kind : std::uint8_t {
        Change,     ///< `key` takes `value`
        Restore,    ///< `key`'s row is restored
        RestoreAll, ///< every row is restored
        Close,      ///< it closed itself: back to the menu
        Bind,       ///< `key`'s binding takes `press`
        Swap,       ///< `key`'s binding takes `press`, `other` takes what it held
        RestoreRow, ///< an action row, confirmed: `key`
    };
    Kind kind = Kind::Close;
    std::string key;
    nlohmann::json value;
    /// Bind, Swap.
    std::optional<input::Binding> press;
    /// Swap: the row of the action that has the press.
    std::string other;

    bool operator==(const SettingsCommand&) const = default;
};
/// "change", "restore", "restore-all", "close", "bind", "swap", "restore-row".
[[nodiscard]] std::string_view toString(SettingsCommand::Kind kind) noexcept;

/// A click of the document (settings_screen.cpp).
struct SettingsClick {
    enum class Kind : std::uint8_t { Tab, Row, Step, Restore, RestoreAll, CaptureCancel, CaptureSwap };
    Kind kind = Kind::Row;
    /// A tab's index among settings::kGroups, a row's among the rows shown.
    std::size_t index = 0;
    /// A step's direction.
    int direction = 1;

    bool operator==(const SettingsClick&) const = default;
};

/// What the application says of a captured press (app::BindingVerdict, which this module does not know).
enum class SettingsCapture : std::uint8_t {
    Free,     ///< it binds at once
    Same,     ///< the action has it: nothing to do, the capture ends
    Clash,    ///< another action has it: a swap is offered
    Reserved, ///< refused: the capture goes on, saying so
    Locked,   ///< refused likewise: a locked action would lose it
};

/// "free", "same", "clash", "reserved", "locked".
[[nodiscard]] std::string_view toString(SettingsCapture answer) noexcept;

class SettingsModel {
public:
    /// Opens on the first group that holds rows, its first row focused.
    void open();
    void close() noexcept;
    /// The rows from now on, the focus kept on its row's key.
    void setRows(std::vector<SettingsRow> rows);

    /// A UI command. The command for the app, if any.
    [[nodiscard]] std::optional<SettingsCommand> apply(UiCommand command);
    [[nodiscard]] std::optional<SettingsCommand> click(const SettingsClick& click);

    /// A capture waits for a press: no UI command is given to apply().
    [[nodiscard]] bool capturing() const noexcept { return m_capture == Capture::Waiting; }
    /// A clash's swap is offered.
    [[nodiscard]] bool offeringSwap() const noexcept { return m_capture == Capture::Offer; }
    /// The row a capture or an offer is for ("" none).
    [[nodiscard]] const std::string& captureKey() const noexcept { return m_captureKey; }
    /// The action that has the press: the offer's other row, or the row that refused it.
    [[nodiscard]] const std::string& captureOther() const noexcept { return m_captureOther; }
    /// Why the last press was refused: "reserved", "locked", "" none.
    [[nodiscard]] const std::string& captureRefused() const noexcept { return m_captureRefused; }
    /// Whole seconds left of the capture.
    [[nodiscard]] int captureSeconds() const noexcept;
    /// A capture's time passes; it ends by itself after settings::kCaptureSeconds.
    void tick(double seconds);
    /// The press a capture read and what it means; `other`: the row that has it. The command for the app, if any.
    [[nodiscard]] std::optional<SettingsCommand> captured(const input::Binding& press, SettingsCapture answer,
                                                          std::string other);
    /// The action rows' confirmation shows.
    [[nodiscard]] bool confirmingAction() const noexcept { return m_confirmAction; }

    [[nodiscard]] bool isOpen() const noexcept { return m_open; }
    /// The group shown (settings::kGroups).
    [[nodiscard]] const std::string& group() const noexcept { return m_group; }
    /// The groups that hold rows, in order.
    [[nodiscard]] std::vector<std::string> groups() const;
    /// The group's rows, in order.
    [[nodiscard]] std::vector<const SettingsRow*> shown() const;
    /// All the rows.
    [[nodiscard]] const std::vector<SettingsRow>& rows() const noexcept { return m_rows; }
    /// The focused position among shown(); shown().size() is "Ripristina tutto".
    [[nodiscard]] std::size_t focus() const noexcept { return m_focus; }
    /// "Ripristina tutto" asks for its confirmation.
    [[nodiscard]] bool confirmingRestoreAll() const noexcept { return m_confirmAll; }
    /// A row's element id ("st-row-<position>"), the focused element's.
    [[nodiscard]] static std::string rowElement(std::size_t position);
    [[nodiscard]] std::string focusedElement() const;
    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }

private:
    void changed() noexcept { ++m_revision; }
    void setGroup(std::string group);
    void moveGroup(int direction);
    void moveFocus(int direction);
    [[nodiscard]] std::optional<SettingsCommand> stepFocused(int direction, bool wrap);
    [[nodiscard]] std::optional<SettingsCommand> restoreFocused();
    [[nodiscard]] std::optional<SettingsCommand> pressRestoreAll();
    [[nodiscard]] std::optional<SettingsCommand> pressRow();
    void startCapture(const SettingsRow& row);
    void endCapture();
    [[nodiscard]] std::optional<SettingsCommand> takeSwap();
    [[nodiscard]] const SettingsRow* focusedRow() const;

    enum class Capture : std::uint8_t { None, Waiting, Offer };

    bool m_open = false;
    bool m_confirmAll = false;
    bool m_confirmAction = false;
    Capture m_capture = Capture::None;
    std::string m_captureKey;
    std::string m_captureOther;
    std::string m_captureRefused;
    std::optional<input::Binding> m_capturePress;
    double m_captureLeft = 0.0;
    std::vector<SettingsRow> m_rows;
    std::string m_group;
    std::size_t m_focus = 0;
    std::uint64_t m_revision = 0;
};

} // namespace rv::ui
