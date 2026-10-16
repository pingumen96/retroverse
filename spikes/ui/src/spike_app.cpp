#include "spike_app.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/StringUtilities.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/app/bindings.hpp>
#include <rv/app/command_line.hpp>
#include <rv/app/scene_graphics.hpp>
#include <rv/app/scene_instance.hpp>
#include <rv/core/frame_clock.hpp>
#include <rv/core/math.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/version.hpp>
#include <rv/input/action_state.hpp>
#include <rv/input/input_script.hpp>
#include <rv/input/input_system.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/platform/sdl_input_collector.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/scene_renderer.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>
#include <rv/ui/in_memory_catalog_source.hpp>
#include <rv/ui/nav_input.hpp>
#include <rv/ui/quick_catalog_model.hpp>
#include <rv/ui/quick_catalog_screen.hpp>
#include <rv/ui/ui_assets.hpp>
#include <rv/ui/ui_command.hpp>
#include <rv/ui/ui_system.hpp>
#include <rv/ui/ui_view.hpp>

#include "allocation_counter.hpp"
#include "mock_catalog.hpp"
#include "scenario.hpp"
#include "spike_report.hpp"
#include "virtual_pad.hpp"

namespace rv::spike_ui {
namespace {

using Clock = std::chrono::steady_clock;
namespace qc = ui::quick_catalog;

/// Developer builds (assets read from the source tree) reload the document when it is saved.
#if defined(RV_SPIKE_WATCH_DOCUMENTS)
constexpr bool kWatchDocuments = true;
#else
constexpr bool kWatchDocuments = false;
#endif

/// Samples kept when the run has no frame limit (--interactive without --frames).
constexpr std::size_t kInteractiveSampleCapacity = 100000;

/// Scenario steps of HighContrast and ExpectHighContrast: the run's setting (--high-contrast) instead of on or off.
constexpr std::uint32_t kRunSetting = 2;

/// The user's bindings file under <user data>/config (app::loadBindings applies it over the defaults).
constexpr std::string_view kUserBindingsFile = "bindings.json";

[[nodiscard]] double millisecondsSince(Clock::time_point start) noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// PNG files directly in `directory` (none when it does not exist).
std::vector<std::filesystem::path> pngFiles(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        if (core::equalsIgnoreCaseAscii(core::pathToUtf8(it->path().extension()), ".png")) {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

/// Moves `from` to `to` (creating its directory), copying when a rename across volumes fails.
bool moveFile(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
    if (to.has_parent_path()) {
        std::filesystem::create_directories(to.parent_path(), error);
    }
    std::filesystem::rename(from, to, error);
    if (!error) {
        return true;
    }
    error.clear();
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        return false;
    }
    std::filesystem::remove(from, error);
    return true;
}

/// The pad an SDL gamepad event comes from; nothing for other events.
[[nodiscard]] std::optional<SDL_JoystickID> gamepadOf(const SDL_Event& event) noexcept {
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return event.gaxis.which;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return event.gbutton.which;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_REMAPPED:
    case SDL_EVENT_GAMEPAD_UPDATE_COMPLETE:
    case SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED:
        return event.gdevice.which;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
        return event.gtouchpad.which;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        return event.gsensor.which;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] bool near(const glm::vec4& a, const glm::vec4& b, float tolerance) noexcept {
    return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance &&
           std::fabs(a.z - b.z) <= tolerance && std::fabs(a.w - b.w) <= tolerance;
}

[[nodiscard]] std::string joined(const std::vector<std::string>& values) {
    std::string text;
    for (const std::string& value : values) {
        text += text.empty() ? value : "," + value;
    }
    return text;
}

[[nodiscard]] std::string chipId(std::size_t index) {
    return std::string(qc::kChipPrefix) + std::to_string(index);
}

[[nodiscard]] std::string rowId(std::size_t slot) {
    return std::string(qc::kRowPrefix) + std::to_string(slot);
}

[[nodiscard]] const char* yesNo(bool value) noexcept {
    return value ? "yes" : "no";
}

[[nodiscard]] std::string positionText(std::optional<std::size_t> position) {
    return position ? std::to_string(*position) : std::string("none");
}

/// The rows of the pool the screen shows for `model`'s window (QuickCatalogScreen: as many as the window, at most
/// kPoolRows), and the element id of the row that shows `position` (the slot of a position is its rest modulo them).
[[nodiscard]] std::size_t poolRowsOf(const ui::QuickCatalogModel& model) noexcept {
    return std::min(model.windowRows(), qc::kPoolRows);
}

[[nodiscard]] std::string rowIdOf(const ui::QuickCatalogModel& model, std::size_t position) {
    return rowId(qc::poolSlotOf(position, poolRowsOf(model)));
}

/// A position and the game on it, recorded by a Mark step.
struct Mark {
    std::size_t position = 0;
    std::string id;
};

/// The title a row of the pool shows (the text of its .game-title), as RmlUi writes it back.
[[nodiscard]] std::string rowTitle(Rml::Element* row) {
    if (row == nullptr) {
        return {};
    }
    Rml::ElementList titles;
    row->GetElementsByClassName(titles, "game-title");
    return titles.empty() ? std::string() : titles.front()->GetInnerRML();
}

/// The elements whose boxes a layout records.
constexpr std::array<std::string_view, 10> kLayoutElements{"catalog", "card",    "hint-bar",      "game-list",
                                                           "search",  "chips",   "chips-box",     "actions",
                                                           "catalog-title", "card-title"};
/// The panels that must stay inside the view at every size.
constexpr std::array<std::string_view, 3> kPanels{"catalog", "card", "hint-bar"};
/// The text elements whose font sizes a layout records (text scaling).
constexpr std::array<std::string_view, 3> kTextElements{"catalog-title", "card-title", "hint-bar"};

} // namespace

struct SpikeApp::State {
    // Declared in the order they are created; destroyed in reverse: the screen before its model, the model before its
    // source, the view before the UI system, the UI before the scene's GPU resources.
    std::unique_ptr<app::SceneGraphics> graphics;
    std::optional<app::SceneInstance> scene;
    scene::Camera camera;
    std::unique_ptr<ui::UiSystem> ui;
    std::unique_ptr<ui::UiView> view;
    std::unique_ptr<ui::InMemoryCatalogSource> source;
    std::unique_ptr<ui::QuickCatalogModel> model;
    std::unique_ptr<ui::QuickCatalogScreen> screen;

    platform::SdlInputCollector collector;
    /// The devices' input through the action map (the default bindings, the user's over them), the input script's
    /// values over it, then the UI's commands.
    input::InputSystem input;
    std::optional<input::InputScriptPlayer> script;
    ui::NavInput nav;
    ui::UiCommandList commands;
    std::optional<VirtualPad> pad;
    bool padFailureLogged = false;
    Scenario scenario;
    ScenarioFacts facts;
    std::size_t nextInputStep = 0;
    std::size_t nextCheckStep = 0;
    std::size_t nextScreenshot = 0;
    /// Selected positions and games recorded by the scenario's Mark steps.
    std::map<std::string, Mark, std::less<>> marks;
    /// The games the last Import step removed, and where they were in the entries.
    std::vector<ui::CatalogEntry> removedGames;
    std::size_t removedAt = 0;
    /// Counts the UI pass's heap allocations (null when it could not be installed).
    std::unique_ptr<AllocationCounter> allocationCounter;

    /// The run's --ui-scale and --high-contrast, which the scenario's steps return to.
    float runTextScale = 1.0f;
    bool runHighContrast = false;
    /// The text scale the view must have now: the run's, times the scenario's factor.
    float textScale = 1.0f;

    std::uint32_t frame = 0;
    /// The window's drawable and its display's content scale this frame (SDL's), and the scale of the frame before.
    glm::ivec2 drawable{0};
    float displayScale = 1.0f;
    float lastDisplayScale = 1.0f;
    double simulatedSeconds = 0.0;
    double inputMs = 0.0;
    /// The screen's work after the context's update (the card settling, the rows that fit, the chips' fades, a lost
    /// focus), counted with the update.
    double afterUpdateMs = 0.0;
    AllocationCounts updateAllocations;
    /// --fps-cap: when the next frame may start (SDL_GetTicksNS), 0 before the first.
    Uint64 nextFrameNs = 0;
    std::optional<double> lastGpuMs;
    std::size_t focusLogged = 0;
    std::size_t actionsLogged = 0;
    /// Whether this frame's update and render handled a change of the UI. A change made before the context's update is
    /// laid out and drawn in the same frame; one made after it (the card settling, the window following the layout in
    /// the screen's afterUpdate) in the next frame, which then counts as the changed one.
    bool focusMovedThisFrame = false;
    bool cardLaidOutThisFrame = false;
    bool rowsBoundThisFrame = false;
    bool selectionMovedThisFrame = false;
    bool focusMovedLate = false;
    bool cardFilledLate = false;
    bool rowsBoundLate = false;
    /// The focus changes, card refreshes and window binds made up to the end of the previous frame's update, and the
    /// selected position then.
    std::size_t focusChangesSeen = 0;
    std::uint64_t cardRefreshesSeen = 0;
    std::uint64_t windowBindsSeen = 0;
    /// Rows of the pool bound to another game up to the end of the previous frame, and in this frame.
    std::uint64_t rowBindsSeen = 0;
    std::uint32_t rowBindsThisFrame = 0;
    std::optional<std::size_t> selectionSeen;
    std::vector<std::string> launched;
    std::vector<std::string> mediaAsked;
    bool infoShown = false;
    ReportData report;

    // --- The screen's events: recorded now, logged after the measured part of the frame --------------------------

    ui::QuickCatalogEvents events() {
        ui::QuickCatalogEvents handlers;
        handlers.focusChanged = [this](const ui::FocusTarget& target) {
            report.focusChanges.push_back(FocusRecord{frame, target.region, target.element});
        };
        handlers.launch = [this](const ui::CatalogEntry& entry) {
            launched.push_back(entry.id);
            report.actions.push_back(ActionRecord{frame, "launch", entry.id});
            screen->setStatus(fmt::format("Avvio di {}: nel prototipo nessun emulatore parte.", entry.title));
        };
        handlers.media = [this](const ui::CatalogEntry& entry) {
            mediaAsked.push_back(entry.id);
            report.actions.push_back(ActionRecord{frame, "media", entry.id});
            screen->setStatus(fmt::format("Media di {}: non disponibili nel prototipo.", entry.title));
        };
        handlers.info = [this](const ui::CatalogEntry& entry, bool shown) {
            infoShown = shown;
            report.actions.push_back(ActionRecord{frame, shown ? "info-shown" : "info-hidden", entry.id});
        };
        handlers.close = [this] {
            report.actions.push_back(ActionRecord{frame, "close", {}});
            screen->hide();
        };
        return handlers;
    }

    /// Logs what the screen recorded during the frame (outside the measured part: logging writes to a file).
    void logRecords() {
        for (; focusLogged < report.focusChanges.size(); ++focusLogged) {
            const FocusRecord& record = report.focusChanges[focusLogged];
            spdlog::info("Frame {}: focus on {} ({})", record.frame, record.element, record.region);
        }
        for (; actionsLogged < report.actions.size(); ++actionsLogged) {
            const ActionRecord& record = report.actions[actionsLogged];
            spdlog::info("Frame {}: {} {}", record.frame, record.action, record.game);
        }
    }

    // --- The scenario ------------------------------------------------------------------------------------------------

    void runInputSteps(app::AppContext& context) {
        const std::vector<Step>& steps = scenario.steps;
        for (; nextInputStep < steps.size() && steps[nextInputStep].frame <= frame; ++nextInputStep) {
            const Step& step = steps[nextInputStep];
            if (isCheck(step.kind) || !pad) {
                continue;
            }
            const SDL_WindowID window = SDL_GetWindowID(context.window().handle());
            bool done = true;
            switch (step.kind) {
            case StepKind::Press:
                done = pad->button(step.button, true);
                break;
            case StepKind::Release:
                done = pad->button(step.button, false);
                break;
            case StepKind::Stick:
                done = pad->axis(SDL_GAMEPAD_AXIS_LEFTX, step.value.x) &&
                       pad->axis(SDL_GAMEPAD_AXIS_LEFTY, step.value.y);
                break;
            case StepKind::Trigger:
                done = pad->axis(step.axis, step.value.x);
                break;
            case StepKind::Resize: {
                const int width = static_cast<int>(step.value.x);
                const int height = static_cast<int>(step.value.y);
                spdlog::info("Frame {}: resizing the window to {}x{}", frame, width, height);
                done = SDL_SetWindowSize(context.window().handle(), width, height);
                break;
            }
            case StepKind::Text: {
                // Delivered with the next frame's events; the step's text outlives it (the scenario lives as long as
                // the run).
                SDL_Event event{};
                event.type = SDL_EVENT_TEXT_INPUT;
                event.text.windowID = window;
                event.text.text = step.text.c_str();
                done = SDL_PushEvent(&event);
                break;
            }
            case StepKind::Key:
                for (std::uint32_t time = 0; time < step.count && done; ++time) {
                    for (const bool down : {true, false}) {
                        SDL_Event event{};
                        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                        event.key.windowID = window;
                        event.key.scancode = step.scancode;
                        event.key.key = step.keycode;
                        event.key.down = down;
                        done = done && SDL_PushEvent(&event);
                    }
                }
                break;
            case StepKind::TextScale:
                textScale = runTextScale * step.value.x;
                spdlog::info("Frame {}: text scale {:.2f}", frame, textScale);
                view->setTextScale(textScale);
                break;
            case StepKind::HighContrast: {
                const bool on = step.count == kRunSetting ? runHighContrast : step.count != 0;
                spdlog::info("Frame {}: high contrast {}", frame, on ? "on" : "off");
                screen->setHighContrast(on);
                break;
            }
            case StepKind::Mark:
                if (const std::optional<std::size_t> position = model->selectedPosition()) {
                    const ui::CatalogEntry* selected = model->selected();
                    marks[step.label] = Mark{*position, selected != nullptr ? selected->id : std::string()};
                    spdlog::info("Frame {}: mark '{}' at position {}", frame, step.label, *position);
                }
                break;
            case StepKind::Import:
                importGames(step);
                break;
            default:
                break;
            }
            if (!done && !padFailureLogged) {
                spdlog::error("Frame {}: scenario step '{}' failed: {}", frame, toString(step.kind), SDL_GetError());
                padFailureLogged = true;
            }
        }
    }

    /// An import under the screen (StepKind::Import): the source's entries replaced, then the model refreshed directly,
    /// as the application does when an import completes; the source answers `step.position` polls later.
    void importGames(const Step& step) {
        if (!model->search().empty() || !model->activeFilter().empty()) {
            spdlog::error("Frame {}: the import step needs the whole list (no search, no filter)", frame);
            return;
        }
        // The whole list is the entries in order: a position is an entry's index.
        std::vector<ui::CatalogEntry> entries(source->entries().begin(), source->entries().end());
        if (step.count > 0) {
            const std::size_t selected = model->selectedPosition().value_or(0);
            removedAt = selected > step.count ? selected - step.count : 0;
            const auto from = entries.begin() + static_cast<std::ptrdiff_t>(removedAt);
            const auto to = from + static_cast<std::ptrdiff_t>(std::min<std::size_t>(step.count, selected));
            removedGames.assign(std::make_move_iterator(from), std::make_move_iterator(to));
            entries.erase(from, to);
        } else {
            entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(removedAt),
                           std::make_move_iterator(removedGames.begin()), std::make_move_iterator(removedGames.end()));
            removedGames.clear();
        }
        spdlog::info("Frame {}: import: {} game(s) {}, the list answered {} poll(s) later", frame,
                     step.count > 0 ? step.count : entries.size() - source->entries().size(),
                     step.count > 0 ? "removed before the selection" : "back", step.position);
        source->setPendingPolls(static_cast<std::uint32_t>(step.position));
        source->replaceEntries(std::move(entries));
        model->refresh();           // as the application does: the screen is not told
        source->setPendingPolls(0); // the query asked runs on; the next ones are answered at once
    }

    void check(std::string name, bool passed, std::string detail) {
        if (passed) {
            spdlog::info("Frame {}: check passed: {} ({})", frame, name, detail);
        } else {
            spdlog::error("Frame {}: check FAILED: {} ({})", frame, name, detail);
        }
        report.checks.push_back(CheckRecord{frame, std::move(name), passed, std::move(detail), false});
    }

    void skip(std::string name, std::string reason) {
        spdlog::warn("Frame {}: check skipped: {} ({})", frame, name, reason);
        report.checks.push_back(CheckRecord{frame, std::move(name), false, std::move(reason), true});
    }

    /// The name of an expectation: "expect-<what>", or "keyboard-<what>" when it verifies keyboard input.
    [[nodiscard]] static std::string named(const Step& step, std::string_view what) {
        return std::string(step.keyboard ? "keyboard-" : "expect-") + std::string(what);
    }

    [[nodiscard]] const LayoutRecord* layoutLabelled(std::string_view label) const {
        for (const LayoutRecord& layout : report.layouts) {
            if (layout.label == label) {
                return &layout;
            }
        }
        return nullptr;
    }

    /// The screen's document (the one that holds the catalog), or null.
    [[nodiscard]] Rml::ElementDocument* screenDocument() const {
        Rml::Element* catalog = view->element("catalog");
        return catalog != nullptr ? catalog->GetOwnerDocument() : nullptr;
    }

    void recordLayout(const std::string& label) {
        LayoutRecord layout;
        layout.frame = frame;
        layout.label = label;
        layout.size = view->size();
        layout.dpRatio = view->dpRatio();
        layout.displayScale = displayScale;
        layout.compact = view->compact();
        layout.windowRows = model->windowRows();
        layout.focused = screen->focus() ? screen->focus()->element : std::string();
        for (const std::string_view id : kLayoutElements) {
            if (const std::optional<glm::vec4> box = view->elementBox(id)) {
                layout.boxes.emplace_back(std::string(id), *box);
            }
        }
        for (const std::string_view id : kTextElements) {
            if (Rml::Element* element = view->element(id)) {
                layout.fontSizes.emplace_back(std::string(id), element->GetComputedValues().font_size());
            }
        }
        const auto boxOf = [&layout](std::string_view id) -> std::optional<glm::vec4> {
            for (const auto& [name, box] : layout.boxes) {
                if (name == id) {
                    return box;
                }
            }
            return std::nullopt;
        };
        if (!layout.focused.empty()) {
            if (const std::optional<glm::vec4> box = view->elementBox(layout.focused)) {
                layout.boxes.emplace_back("focused:" + layout.focused, *box);
            }
        }
        // Every panel inside the view.
        constexpr float kTolerance = 1.0f;
        bool inside = true;
        std::string outside;
        for (const std::string_view id : kPanels) {
            const std::optional<glm::vec4> box = boxOf(id);
            if (!box || box->x < -kTolerance || box->y < -kTolerance ||
                box->x + box->z > static_cast<float>(layout.size.x) + kTolerance ||
                box->y + box->w > static_cast<float>(layout.size.y) + kTolerance) {
                inside = false;
                outside += outside.empty() ? std::string(id) : ", " + std::string(id);
            }
        }
        check("layout " + label + ": panels inside the view", inside,
              inside ? fmt::format("{}x{} pixels, dp ratio {:.2f}", layout.size.x, layout.size.y, layout.dpRatio)
                     : "outside: " + outside);
        // The card's buttons inside their row: the region hides what overflows it, and RmlUi never scrolls a hidden
        // region to the focus, so a button outside it could not be seen when focused.
        if (const std::optional<glm::vec4> row = boxOf("actions")) {
            std::string clipped;
            for (const std::string_view id : {qc::kPlayId, qc::kMediaId, qc::kInfoId}) {
                const std::optional<glm::vec4> button = view->elementBox(id);
                if (!button || button->x < row->x - kTolerance || button->y < row->y - kTolerance ||
                    button->x + button->z > row->x + row->z + kTolerance ||
                    button->y + button->w > row->y + row->w + kTolerance) {
                    clipped += clipped.empty() ? std::string(id) : ", " + std::string(id);
                }
            }
            check("layout " + label + ": the card's actions inside their row", clipped.empty(),
                  clipped.empty() ? std::string("every button whole") : "clipped: " + clipped);
        }
        // The focused game's row inside the list area.
        if (layout.focused.starts_with(qc::kRowPrefix)) {
            const std::optional<glm::vec4> list = boxOf(qc::kListId);
            const std::optional<glm::vec4> focused = view->elementBox(layout.focused);
            const bool visible = list && focused && focused->y >= list->y - kTolerance &&
                                 focused->y + focused->w <= list->y + list->w + kTolerance;
            check("layout " + label + ": focused game visible in the list", visible,
                  fmt::format("{}, {} row(s) shown", layout.focused, layout.windowRows));
        }
        // RmlUi follows the window: its context has the window's drawable (SDL's) and the dp ratio of the display's scale
        // times the text scale asked for, its compact theme follows the display alone, its document is laid out at the
        // drawable's size (no scale drift across resizes).
        Rml::Context& rml = view->context();
        const Rml::Vector2i dimensions = rml.GetDimensions();
        const float ratio = rml.GetDensityIndependentPixelRatio();
        const float expectedRatio = displayScale * textScale;
        const bool compact = rml.IsThemeActive(Rml::String(ui::kCompactTheme));
        const bool compactExpected = ui::compactLayout(drawable.y, displayScale);
        Rml::ElementDocument* document = screenDocument();
        const Rml::Vector2f documentSize =
            document != nullptr ? document->GetBox().GetSize(Rml::BoxArea::Border) : Rml::Vector2f(0.0f, 0.0f);
        const bool follows = dimensions.x == drawable.x && dimensions.y == drawable.y &&
                             std::fabs(ratio - expectedRatio) < 1.0e-4f && compact == compactExpected &&
                             std::fabs(documentSize.x - static_cast<float>(drawable.x)) <= 0.5f &&
                             std::fabs(documentSize.y - static_cast<float>(drawable.y)) <= 0.5f;
        check("layout " + label + ": RmlUi follows the window", follows,
              fmt::format("context {}x{} at dp ratio {:.3f}{}, document {:.0f}x{:.0f}; drawable {}x{}, display scale "
                          "{:.2f} x text scale {:.2f}{}",
                          dimensions.x, dimensions.y, ratio, compact ? ", compact theme" : "", documentSize.x,
                          documentSize.y, drawable.x, drawable.y, displayScale, textScale,
                          compactExpected ? ", compact expected" : ""));
        report.layouts.push_back(std::move(layout));
    }

    void compareLayouts(const std::string& label, const std::string& reference) {
        const LayoutRecord* now = layoutLabelled(label);
        const LayoutRecord* before = layoutLabelled(reference);
        if (now == nullptr || before == nullptr) {
            check("same-layout " + label + " = " + reference, false, "a layout was not recorded");
            return;
        }
        if (now->size != before->size || now->dpRatio != before->dpRatio) {
            // The window could not get its first drawable back, or the display's scale changed (another program changed
            // the display mode during the run): the layouts cannot be compared.
            skip("same-layout " + label + " = " + reference,
                 fmt::format("the view is {}x{} at dp ratio {:.2f}, it was {}x{} at {:.2f}", now->size.x, now->size.y,
                             now->dpRatio, before->size.x, before->size.y, before->dpRatio));
            return;
        }
        std::string differences;
        for (const auto& [id, box] : before->boxes) {
            const auto match = std::find_if(now->boxes.begin(), now->boxes.end(),
                                            [&id](const auto& entry) { return entry.first == id; });
            if (match == now->boxes.end() || !near(match->second, box, 0.5f)) {
                differences += differences.empty() ? id : ", " + id;
            }
        }
        if (now->windowRows != before->windowRows) {
            differences += fmt::format("{}{} row(s) shown, {} before", differences.empty() ? "" : ", ", now->windowRows,
                                       before->windowRows);
        }
        check("same-layout " + label + " = " + reference, differences.empty(),
              differences.empty() ? fmt::format("{} boxes equal at {}x{}, {} row(s) shown", before->boxes.size(),
                                                now->size.x, now->size.y, now->windowRows)
                                  : "differ: " + differences);
    }

    /// Text scaling: every recorded font size of layout `label` is `ratio` times that of layout `reference` (5%), in
    /// the same layout theme.
    void compareTextSizes(const std::string& label, const std::string& reference, float ratio) {
        const std::string name = fmt::format("text-scale {} = {} x{:.2f}", label, reference, ratio);
        const LayoutRecord* now = layoutLabelled(label);
        const LayoutRecord* before = layoutLabelled(reference);
        if (now == nullptr || before == nullptr || before->fontSizes.empty()) {
            check(name, false, "a layout or its font sizes were not recorded");
            return;
        }
        if (now->size != before->size || now->displayScale != before->displayScale) {
            // Another program changed the display mode between the two layouts: they cannot be compared.
            skip(name, fmt::format("the view is {}x{} at display scale {:.2f}, it was {}x{} at {:.2f}", now->size.x,
                                   now->size.y, now->displayScale, before->size.x, before->size.y,
                                   before->displayScale));
            return;
        }
        bool larger = now->compact == before->compact;
        std::string detail;
        for (const auto& [id, size] : before->fontSizes) {
            const auto match = std::find_if(now->fontSizes.begin(), now->fontSizes.end(),
                                            [&id](const auto& entry) { return entry.first == id; });
            const float scaled = match != now->fontSizes.end() ? match->second : 0.0f;
            const float measured = size > 0.0f ? scaled / size : 0.0f;
            larger = larger && std::fabs(measured - ratio) <= 0.05f * ratio;
            detail += fmt::format("{}{} {:.1f} -> {:.1f} px (x{:.3f})", detail.empty() ? "" : ", ", id, size, scaled,
                                  measured);
        }
        detail += fmt::format("; layout theme {} -> {}, {} -> {} row(s) shown", before->compact ? "compact" : "regular",
                              now->compact ? "compact" : "regular", before->windowRows, now->windowRows);
        check(name, larger, detail);
    }

    /// The pool shows the model's window: a row per window row and none more, the row of each position (its rest modulo
    /// the rows) showing its game's title (or blank past the list's end), the rows in the window's order a pitch apart
    /// from the list area's top, filling it (the last one inside it, one more would not fit), and the scroll thumb
    /// where the window lies in the list.
    void checkWindow(const Step& step) {
        constexpr float kTolerance = 1.0f;
        const std::span<const ui::CatalogEntry> window = model->window();
        const std::size_t rows = poolRowsOf(*model);
        const std::size_t first = model->windowFirst();
        std::string problems;
        const auto problem = [&problems](const std::string& text) {
            if (problems.size() < 400) {
                problems += problems.empty() ? text : "; " + text;
            }
        };
        for (std::size_t slot = rows; slot < qc::kPoolRows; ++slot) {
            if (view->element(rowId(slot)) != nullptr) {
                problem(rowId(slot) + " exists past the pool's " + std::to_string(rows) + " row(s)");
            }
        }
        std::optional<glm::vec4> previous;
        float pitch = 0.0f;
        for (std::size_t row = 0; row < rows; ++row) {
            const std::string id = rowIdOf(*model, first + row);
            Rml::Element* element = view->element(id);
            if (element == nullptr) {
                problem(id + " missing");
                continue;
            }
            const bool displayed = element->GetComputedValues().display() != Rml::Style::Display::None;
            const bool vacant = element->IsClassSet("vacant");
            if (!displayed) {
                problem(id + " not displayed");
            } else if (row < window.size()) {
                const std::string expected = Rml::StringUtilities::EncodeRml(window[row].title);
                if (vacant || rowTitle(element) != expected) {
                    problem(fmt::format("{} (window row {}) shows '{}', the window has '{}'", id, row,
                                        rowTitle(element), expected));
                }
            } else if (!vacant) {
                problem(id + " not blank past the list's end");
            }
            // In the window's order, a pitch apart.
            const std::optional<glm::vec4> box = view->elementBox(id);
            if (box && previous) {
                const float gap = box->y - previous->y;
                if (row == 1) {
                    pitch = gap;
                }
                if (gap <= 0.0f || std::fabs(gap - pitch) > kTolerance) {
                    problem(fmt::format("window row {} ({}) {:.1f} px below the one before, {:.1f} expected", row, id,
                                        gap, pitch));
                }
            }
            previous = box;
        }
        // The rows fill the list area from its top.
        Rml::Element* list = view->element(qc::kListId);
        const std::optional<glm::vec4> top = view->elementBox(rowIdOf(*model, first));
        const std::optional<glm::vec4> last = view->elementBox(rowIdOf(*model, first + rows - 1));
        if (list != nullptr && top && last) {
            const float areaTop = list->GetAbsoluteOffset(Rml::BoxArea::Content).y;
            const float areaBottom = areaTop + list->GetBox().GetSize(Rml::BoxArea::Content).y;
            if (std::fabs(top->y - areaTop) > kTolerance) {
                problem(
                    fmt::format("the window's first row at {:.1f}, the list area's top at {:.1f}", top->y, areaTop));
            }
            if (last->y + last->w > areaBottom + kTolerance) {
                problem(fmt::format("the last row ends at {:.1f}, past the list area's {:.1f}", last->y + last->w,
                                    areaBottom));
            }
            if (rows >= 2 && rows < qc::kPoolRows && last->y + pitch + last->w <= areaBottom - kTolerance) {
                problem(fmt::format("one more row would fit (the last ends at {:.1f}, the area at {:.1f})",
                                    last->y + last->w, areaBottom));
            }
        }
        // The window holds the selection.
        const std::optional<std::size_t> slot = model->selectedSlot();
        if (model->count() > 0 && (!slot || *slot >= rows)) {
            problem("the selection is outside the window");
        }
        // The scroll thumb.
        const bool scrolls = model->count() > rows;
        const std::optional<glm::vec4> track = view->elementBox("list-track");
        const std::optional<glm::vec4> thumb = view->elementBox(qc::kScrollThumbId);
        const bool trackShown = view->element("list-track") != nullptr &&
                                view->element("list-track")->GetComputedValues().display() != Rml::Style::Display::None;
        std::string thumbDetail = "no thumb";
        if (scrolls != trackShown) {
            problem(fmt::format("the scroll indicator {} with {} game(s) in {} row(s)", trackShown ? "shows" : "hides",
                                model->count(), rows));
        } else if (scrolls && track && thumb && track->w > thumb->w) {
            const float place = (thumb->y - track->y) / (track->w - thumb->w);
            const float expected = static_cast<float>(model->windowFirst()) / static_cast<float>(model->count() - rows);
            thumbDetail = fmt::format("thumb at {:.3f}, window at {:.3f}", place, expected);
            if (std::fabs(place - expected) > 0.01f) {
                problem(thumbDetail);
            }
        }
        check(named(step, "window"), problems.empty(),
              problems.empty() ? fmt::format("{} row(s) from position {} of {}, {:.1f} px apart, {}", rows,
                                             model->windowFirst(), model->count(), pitch, thumbDetail)
                               : problems);
    }

    void runChecks() {
        const std::vector<Step>& steps = scenario.steps;
        for (; nextCheckStep < steps.size() && steps[nextCheckStep].frame <= frame; ++nextCheckStep) {
            const Step& step = steps[nextCheckStep];
            if (!isCheck(step.kind)) {
                continue;
            }
            runCheck(step);
        }
    }

    void runCheck(const Step& step) {
        constexpr float kTolerance = 1.0f;
        switch (step.kind) {
        case StepKind::ExpectFocus: {
            const std::string actual = screen->focus() ? screen->focus()->element : std::string("(none)");
            check(named(step, "focus " + step.text), actual == step.text, "focus on " + actual);
            break;
        }
        case StepKind::ExpectSteady: {
            const auto moves = static_cast<std::uint32_t>(
                std::count_if(report.selections.begin(), report.selections.end(),
                              [&step](const SelectionRecord& record) { return record.frame >= step.since; }));
            const auto focusMoves = static_cast<std::uint32_t>(
                std::count_if(report.focusChanges.begin(), report.focusChanges.end(),
                              [&step](const FocusRecord& record) { return record.frame >= step.since; }));
            check(named(step, fmt::format("steady since frame {}", step.since)),
                  moves == step.count && focusMoves == step.count,
                  fmt::format("{} move(s) of the selection and {} of the focus, {} expected", moves, focusMoves,
                              step.count));
            break;
        }
        case StepKind::ExpectLaunched:
            check(named(step, "launched " + step.text), joined(launched) == step.text, "launched: " + joined(launched));
            break;
        case StepKind::ExpectMedia:
            check(named(step, "media " + step.text), joined(mediaAsked) == step.text, "media: " + joined(mediaAsked));
            break;
        case StepKind::ExpectInfo:
            check(named(step, "info " + step.text), (infoShown ? "shown" : "hidden") == step.text,
                  infoShown ? "details shown" : "details hidden");
            break;
        case StepKind::ExpectHistory: {
            const std::size_t depth = screen->history().depth();
            check(named(step, fmt::format("history {}", step.count)), depth == step.count,
                  fmt::format("{} entr{}", depth, depth == 1 ? "y" : "ies"));
            break;
        }
        case StepKind::ExpectCount:
            check(named(step, fmt::format("count {}", step.position)), model->count() == step.position,
                  fmt::format("{} game(s) listed", model->count()));
            break;
        case StepKind::ExpectSelected: {
            const std::optional<std::size_t> position = model->selectedPosition();
            const ui::CatalogEntry* selected = model->selected();
            const std::string id = selected != nullptr ? selected->id : std::string("(no game)");
            const bool passed = position == step.position && (step.text.empty() || id == step.text);
            check(named(step, fmt::format("selected {}{}", step.position, step.text.empty() ? "" : " " + step.text)),
                  passed,
                  fmt::format("selection at {} ({}) of {} game(s), window from {}", positionText(position), id,
                              model->count(), model->windowFirst()));
            break;
        }
        case StepKind::ExpectPages: {
            const auto mark = marks.find(step.label);
            const std::optional<std::size_t> position = model->selectedPosition();
            if (mark == marks.end() || model->count() == 0) {
                check(named(step, fmt::format("pages {} from {}", step.pages, step.label)), false, "no such mark");
                break;
            }
            const auto shift =
                static_cast<std::ptrdiff_t>(step.pages) * static_cast<std::ptrdiff_t>(model->windowRows());
            const auto target = std::clamp<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(mark->second.position) + shift,
                                                           0, static_cast<std::ptrdiff_t>(model->count()) - 1);
            check(named(step, fmt::format("pages {} from {}", step.pages, step.label)),
                  position == static_cast<std::size_t>(target),
                  fmt::format("selection at {}, {} expected ({} + {} x {} row(s))", positionText(position), target,
                              mark->second.position, step.pages, model->windowRows()));
            break;
        }
        case StepKind::ExpectMarked: {
            // The game marked, at its marked position moved by `pages` (an import removed games before it).
            const auto mark = marks.find(step.label);
            const std::string name = named(step, fmt::format("marked {} {:+}", step.label, step.pages));
            if (mark == marks.end()) {
                check(name, false, "no such mark");
                break;
            }
            const std::optional<std::size_t> position = model->selectedPosition();
            const ui::CatalogEntry* selected = model->selected();
            const std::string id = selected != nullptr ? selected->id : std::string("(no game)");
            const std::ptrdiff_t expected = static_cast<std::ptrdiff_t>(mark->second.position) + step.pages;
            check(name, position && static_cast<std::ptrdiff_t>(*position) == expected && id == mark->second.id,
                  fmt::format("selection at {} ({}) of {} game(s), the marked game {} expected at {}",
                              positionText(position), id, model->count(), mark->second.id, expected));
            break;
        }
        case StepKind::ExpectRow: {
            // The focus on the selected game's row (the row of its position), which shows that game and carries the
            // class "selected".
            const std::optional<std::size_t> position = model->selectedPosition();
            const ui::CatalogEntry* selected = model->selected();
            const std::string expected = position ? rowIdOf(*model, *position) : std::string("(no row)");
            const std::string focused = screen->focus() ? screen->focus()->element : std::string("(none)");
            Rml::Element* row = position ? view->element(expected) : nullptr;
            const std::string title = rowTitle(row);
            const std::string wanted =
                selected != nullptr ? Rml::StringUtilities::EncodeRml(selected->title) : std::string("(no game)");
            const bool marked = row != nullptr && row->IsClassSet(Rml::String(qc::kSelectedClass));
            check(named(step, "row of the selection"), focused == expected && marked && title == wanted,
                  fmt::format("focus on {}, the selection's row {} (class {}), showing '{}', the selection '{}'",
                              focused, expected, marked ? "set" : "missing", title, wanted));
            break;
        }
        case StepKind::ExpectWindow:
            checkWindow(step);
            break;
        case StepKind::ExpectCard: {
            // The title the document shows, against the game the actions act on.
            const ui::CatalogEntry* selected = model->selected();
            const std::string expected =
                selected != nullptr ? Rml::StringUtilities::EncodeRml(selected->title) : std::string("(no game)");
            Rml::Element* title = view->element(qc::kCardTitleId);
            const std::string shown = title != nullptr ? title->GetInnerRML() : std::string("(no card)");
            check(named(step, "card shows the selected game"), shown == expected,
                  fmt::format("card '{}', selected '{}'", shown, expected));
            break;
        }
        case StepKind::ExpectInView: {
            const std::optional<glm::vec4> box = view->elementBox(step.text);
            const std::optional<glm::vec4> container = view->elementBox(step.label);
            const bool inView = box && container && box->x >= container->x - kTolerance &&
                                box->y >= container->y - kTolerance &&
                                box->x + box->z <= container->x + container->z + kTolerance &&
                                box->y + box->w <= container->y + container->w + kTolerance;
            check(named(step, step.text + " in view in " + step.label), inView,
                  box && container ? fmt::format("{} from x {:.0f} to {:.0f}, {} from x {:.0f} to {:.0f}", step.text,
                                                 box->x, box->x + box->z, step.label, container->x,
                                                 container->x + container->z)
                                   : std::string("an element is missing"));
            break;
        }
        case StepKind::ExpectClass: {
            Rml::Element* element = view->element(step.text);
            const bool wanted = step.count != 0;
            const bool set = element != nullptr && element->IsClassSet(step.label);
            check(named(step, fmt::format("{} {} {}", step.text, wanted ? "has" : "lacks", step.label)),
                  element != nullptr && set == wanted,
                  element == nullptr ? std::string("no such element") : std::string(set ? "class set" : "not set"));
            break;
        }
        case StepKind::ExpectOverflowCue: {
            // Which chips lie beyond the row's edges, from their boxes, against the fades the screen shows.
            const std::size_t count = model->filters().size();
            const std::optional<glm::vec4> row = view->elementBox(qc::kChipsId);
            const std::optional<glm::vec4> first = count > 0 ? view->elementBox(chipId(0)) : std::nullopt;
            const std::optional<glm::vec4> last = count > 0 ? view->elementBox(chipId(count - 1)) : std::nullopt;
            Rml::Element* box = view->element(qc::kChipsBoxId);
            if (!row || !first || !last || box == nullptr) {
                check(named(step, "chips fades"), false, "the chips row is missing");
                break;
            }
            const bool hiddenBefore = first->x < row->x - kTolerance;
            const bool hiddenAfter = last->x + last->z > row->x + row->z + kTolerance;
            const bool fadeBefore = box->IsClassSet(Rml::String(qc::kMoreBeforeClass));
            const bool fadeAfter = box->IsClassSet(Rml::String(qc::kMoreAfterClass));
            check(named(step, "chips fades"), fadeBefore == hiddenBefore && fadeAfter == hiddenAfter,
                  fmt::format("chips beyond the start {}, beyond the end {}; fades at the start {}, at the end {}",
                              yesNo(hiddenBefore), yesNo(hiddenAfter), yesNo(fadeBefore), yesNo(fadeAfter)));
            break;
        }
        case StepKind::ExpectHighContrast: {
            const bool wanted = step.count == kRunSetting ? runHighContrast : step.count != 0;
            Rml::Element* catalog = view->element("catalog");
            const Rml::ElementDocument* document = screenDocument();
            const bool classSet =
                document != nullptr && document->IsClassSet(Rml::String(qc::kHighContrastClass));
            const Rml::Colourb background =
                catalog != nullptr ? catalog->GetComputedValues().background_color() : Rml::Colourb(0, 0, 0, 0);
            const bool black = background == Rml::Colourb(0, 0, 0, 255);
            check(fmt::format("high-contrast {}", wanted ? "on" : "off"),
                  document != nullptr && classSet == wanted && black == wanted,
                  fmt::format("document class {}, catalog background #{:02X}{:02X}{:02X}{:02X}",
                              classSet ? "set" : "not set", static_cast<int>(background.red),
                              static_cast<int>(background.green), static_cast<int>(background.blue),
                              static_cast<int>(background.alpha)));
            break;
        }
        case StepKind::Layout:
            recordLayout(step.label);
            break;
        case StepKind::SameLayout:
            compareLayouts(step.label, step.text);
            break;
        case StepKind::LargerText:
            compareTextSizes(step.label, step.text, step.value.x);
            break;
        default:
            break;
        }
    }

    /// Scenario checks after the step the run reached: they never ran.
    [[nodiscard]] std::size_t checksNotRun() const {
        return static_cast<std::size_t>(std::count_if(scenario.steps.begin() + static_cast<std::ptrdiff_t>(nextCheckStep),
                                                      scenario.steps.end(),
                                                      [](const Step& step) { return isCheck(step.kind); }));
    }

    /// With the catalog closed (Back at its root): Y or A opens it again, Back quits.
    void whileClosed(app::AppContext& context) {
        for (const ui::UiCommand command : commands.commands()) {
            if (command == ui::UiCommand::Media || command == ui::UiCommand::Activate) {
                screen->show();
                nav.reset();
                report.actions.push_back(ActionRecord{frame, "open", {}});
                return;
            }
            if (command == ui::UiCommand::Back) {
                spdlog::info("Back with the catalog closed: quitting");
                context.requestQuit();
                return;
            }
        }
    }

    /// The screenshot asked for this frame, if any.
    [[nodiscard]] const ScreenshotRequest* captureFor(const std::vector<ScreenshotRequest>& requests) {
        while (nextScreenshot < requests.size() && requests[nextScreenshot].frame < frame) {
            ++nextScreenshot; // a frame the run skipped (never: frames are consecutive)
        }
        return nextScreenshot < requests.size() && requests[nextScreenshot].frame == frame ? &requests[nextScreenshot]
                                                                                            : nullptr;
    }

    /// --fps-cap: waits until the frame's slot starts (precisely: SDL_DelayPrecise), so the GPU never queues frames
    /// ahead; a frame later than one period restarts the pacing from now.
    void pace(int framesPerSecond) {
        if (framesPerSecond <= 0) {
            return;
        }
        const Uint64 period = 1000000000ull / static_cast<Uint64>(framesPerSecond);
        const Uint64 now = SDL_GetTicksNS();
        if (nextFrameNs == 0 || now > nextFrameNs + period) {
            nextFrameNs = now + period;
            return;
        }
        if (nextFrameNs > now) {
            SDL_DelayPrecise(nextFrameNs - now);
        }
        nextFrameNs += period;
    }

    [[nodiscard]] AllocationCounts takeAllocations() noexcept {
        return allocationCounter ? allocationCounter->take() : AllocationCounts{};
    }

    void recordSample(app::AppContext& context) {
        FrameSample sample;
        sample.frame = frame;
        sample.inputMs = inputMs;
        sample.updateMs = view->times().updateMs + afterUpdateMs;
        sample.renderMs = view->times().renderMs;
        sample.beginFrameMs = view->times().beginFrameMs;
        sample.contextRenderMs = view->times().contextRenderMs;
        sample.endFrameMs = view->times().endFrameMs;
        sample.drawCalls = view->times().drawCalls;
        // A new GPU result, told by its value: gl::GpuTimer reports the latest result only, so a result equal to the
        // previous one (GL_TIME_ELAPSED counts in steps of 32 ns, and a quiet UI frame costs the same every time) and a
        // result followed by another within the same frame are not recorded (spike report: a few percent of the frames).
        if (const std::optional<double> gpu = view->times().gpuMs; gpu && gpu != lastGpuMs) {
            sample.gpuMs = *gpu;
            lastGpuMs = gpu;
        }
        sample.frameMs = context.clock().deltaSeconds() * 1000.0;
        sample.focusMoved = focusMovedThisFrame;
        sample.cardRefreshed = cardLaidOutThisFrame;
        sample.rowsBound = rowsBoundThisFrame;
        sample.rowBinds = rowBindsThisFrame;
        sample.selectionMoved = selectionMovedThisFrame;
        sample.updateAllocations = updateAllocations;
        sample.renderAllocations = takeAllocations();
        if (report.samples.size() < report.samples.capacity()) {
            report.samples.push_back(sample); // reserved at start: no allocation per frame
        }
    }
};

SpikeApp::SpikeApp(SpikeOptions options) : m_options(std::move(options)) {}

SpikeApp::~SpikeApp() = default;

Status SpikeApp::onInit(app::AppContext& context) {
    auto state = std::make_unique<State>();
    State& s = *state;
    const core::Paths& paths = context.paths();

    // The 3D background: the test room through the unchanged rv_app pieces.
    auto graphics = app::SceneGraphics::create(context);
    if (!graphics) {
        return propagate(graphics.error());
    }
    s.graphics = std::move(*graphics);
    const std::filesystem::path scenePath = app::resolveScenePath(m_options.run.scene, paths.assetRoot());
    auto description = scene::loadSceneDescription(scenePath);
    if (!description) {
        return propagate(description.error());
    }
    auto instance = app::SceneInstance::create(*description, s.graphics->materialLibrary(),
                                               scene::assetModelLoader(paths.assetRoot()), s.graphics->resources());
    if (!instance) {
        return propagate(instance.error());
    }
    s.scene.emplace(std::move(*instance));
    s.camera.setLens(description->camera.lens);
    s.camera.setPose(description->camera.position, core::degreesToRadians(description->camera.yawDeg),
                     core::degreesToRadians(description->camera.pitchDeg));

    // The catalog: 100,000 generated games in memory, the source a virtualized list pages.
    const Clock::time_point generation = Clock::now();
    s.source = std::make_unique<ui::InMemoryCatalogSource>(mockCatalog());
    s.report.catalogGames = s.source->count();
    s.report.catalogMs = millisecondsSince(generation);
    spdlog::info("Mock catalog: {} game(s) generated and indexed in {:.0f} ms ({} rejected)", s.report.catalogGames,
                 s.report.catalogMs, s.source->rejected());
    if (m_options.scenario && m_options.scenarioKind == ScenarioKind::QuickCatalog) {
        auto facts = readScenarioFacts(*s.source, kMarkerWord);
        if (!facts) {
            return propagate(facts.error());
        }
        s.facts = std::move(*facts);
        spdlog::info("Scenario facts: marker {} at position {} of {}, word '{}' ({} game(s), the marker at {}), {} "
                     "chip(s)",
                     s.facts.markerId, s.facts.marker, s.facts.count, s.facts.markerWord, s.facts.markerWordCount,
                     s.facts.markerInWord, s.facts.chips);
    }

    // The input: the bindings of every Retroverse application (the defaults, the user's file over them), so that the
    // UI actions reach the screen as the user bound them; the input script's values over them.
    input::ActionMap bindings = app::loadBindings(paths);
    const std::filesystem::path userBindings = paths.configDir() / core::pathFromUtf8(kUserBindingsFile);
    std::error_code bindingsError;
    if (std::filesystem::is_regular_file(userBindings, bindingsError)) {
        s.report.userBindings = core::pathToGenericUtf8(userBindings);
    }
    s.input.setMap(bindings);
    if (m_options.inputScript) {
        auto script = input::InputScript::loadFromFile(*m_options.inputScript);
        if (!script) {
            return propagate(script.error());
        }
        spdlog::info("Input script {}: {} step(s) up to frame {}", core::pathToGenericUtf8(*m_options.inputScript),
                     script->steps().size(), script->lastFrame());
        s.script.emplace(std::move(*script));
    }

    // The product UI.
    auto ui = ui::UiSystem::create(context.window(), ui::UiSystemConfig{paths.assetRoot()});
    if (!ui) {
        return propagate(ui.error());
    }
    s.ui = std::move(*ui);
    auto view = ui::UiView::create(*s.ui, "window", context.framebufferSize(), context.window().displayScale());
    if (!view) {
        return propagate(view.error());
    }
    s.view = std::move(*view);
    s.view->setCommandKeys(ui::commandKeys(bindings));
    s.runTextScale = m_options.textScale;
    s.runHighContrast = m_options.highContrast;
    s.textScale = m_options.textScale;
    s.view->setTextScale(m_options.textScale);
    s.model = std::make_unique<ui::QuickCatalogModel>(*s.source);
    auto screen = ui::QuickCatalogScreen::create(*s.view, *s.model, s.events(), kWatchDocuments);
    if (!screen) {
        return propagate(screen.error());
    }
    s.screen = std::move(*screen);
    s.screen->setHighContrast(m_options.highContrast);
    s.graphics->setOverlay(
        [this](gl::GLDevice& device) {
            AllocationCounter::begin();
            m_state->view->render(device);
            AllocationCounter::end();
        },
        [this](gl::GLDevice& device, const gl::Framebuffer& target) { m_state->view->renderTo(device, target); });

    // The UI pass's heap allocations, Release builds included.
    if (auto counter = AllocationCounter::install(); counter) {
        s.allocationCounter = std::move(*counter);
        s.report.allocationCounting =
            fmt::format("calls to the CRT's allocation functions from spike_ui.exe and rmlui.dll on the main thread "
                        "({} import table entries patched), {} build",
                        s.allocationCounter->patchedEntries(), core::kBuildType);
        spdlog::info("Heap allocations of the UI pass counted: {}", s.report.allocationCounting);
    } else {
        s.report.allocationCounting = counter.error().toString();
        spdlog::warn("Heap allocations of the UI pass not counted: {}", s.report.allocationCounting);
    }

    // Input devices: every connected pad by hand (--interactive); the scripted virtual pad alone in scenario runs, and
    // no pad at all with an input script, so that a pad in use on the machine cannot move the selection under the
    // checks.
    const std::optional<std::uint32_t> frameLimit = m_options.run.frameLimit;
    if (m_options.scenario) {
        // The scripted pad must reach a window that never has the focus (hidden runs): background joystick events are
        // allowed for this run only (SdlPlatform disables them so that an emulator's pad input never reaches us).
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        auto pad = VirtualPad::attach("Retroverse Spike Pad");
        if (!pad) {
            return propagate(pad.error());
        }
        s.pad.emplace(std::move(*pad));
        s.scenario =
            m_options.scenarioKind == ScenarioKind::QuickCatalog ? defaultScenario(s.facts) : userBindingsScenario();
        if (!frameLimit || *frameLimit < s.scenario.frames) {
            spdlog::warn("The scenario needs {} frames; with {} the checks after the last frame never run and the run "
                         "fails",
                         s.scenario.frames, frameLimit ? std::to_string(*frameLimit) : std::string("no limit"));
        }
    } else if (!s.script) {
        s.collector.gamepads().openConnected();
    }
    s.report.samples.reserve(frameLimit ? *frameLimit : kInteractiveSampleCapacity);
    s.report.focusChanges.reserve(frameLimit ? *frameLimit / 2 + 64 : 4096);
    s.report.selections.reserve(frameLimit ? *frameLimit + 64 : 4096);
    s.report.actions.reserve(256);

    ReportData& report = s.report;
    report.build = std::string(core::kBuildType);
    report.rmluiVersion = Rml::GetVersion();
    report.glRenderer = context.device().caps().renderer;
    report.glVersion = context.device().caps().version;
    report.initialSize = context.framebufferSize();
    report.displayScale = context.window().displayScale();
    s.displayScale = report.displayScale;
    s.lastDisplayScale = report.displayScale;
    report.textScale = m_options.textScale;
    report.highContrast = m_options.highContrast;
    report.scenario = m_options.scenario;
    report.scenarioName = m_options.scenario ? std::string(toString(m_options.scenarioKind))
                                             : (s.script ? std::string("input-script") : std::string("interactive"));
    report.fixedDeltaSeconds = m_options.run.fixedDeltaSeconds;
    report.vsync = m_options.run.vsync;
    report.fpsCap = m_options.fpsCap;
    report.fonts = s.ui->loadedFontFaces();
    report.phases = s.scenario.phases;
    report.poolRows = qc::kPoolRows;
    spdlog::info("spike_ui: {}x{} pixels, display scale {:.2f}, scenario {} ({} scripted step(s) over {} frame(s)), {} "
                 "screenshot(s) asked, {} row(s) shown",
                 report.initialSize.x, report.initialSize.y, report.displayScale, report.scenarioName,
                 s.scenario.steps.size(), s.scenario.frames, m_options.screenshots.size(), s.model->windowRows());
    s.logRecords(); // the first focus
    s.focusChangesSeen = report.focusChanges.size(); // laid out by the screen's creation
    s.cardRefreshesSeen = s.screen->cardRefreshes();
    s.windowBindsSeen = s.screen->windowBinds();
    s.rowBindsSeen = s.screen->rowBinds();
    s.selectionSeen = s.model->selectedPosition();
    m_state = std::move(state);
    return ok();
}

void SpikeApp::onEvent(app::AppContext& /*context*/, const SDL_Event& event) {
    if (!m_state) {
        return;
    }
    State& s = *m_state;
    if (s.pad || s.script) {
        if (const std::optional<SDL_JoystickID> pad = gamepadOf(event); pad && (!s.pad || *pad != s.pad->id())) {
            return; // a real pad: scripted runs hear the virtual one only, input-script runs none
        }
    }
    // The UI first (mouse, text, keys that are not commands); what it does not use goes to the input collector, which
    // turns pads and command keys into the raw frame the bindings resolve.
    if (s.view->handleEvent(event)) {
        return;
    }
    s.collector.handleEvent(event);
}

void SpikeApp::onUpdate(app::AppContext& context, double deltaSeconds) {
    if (!m_state) {
        return;
    }
    State& s = *m_state;
    // The frame starts when its slot does (--fps-cap): the previous frame's swap has submitted all its commands, so
    // no GPU span measured this frame waits on a later flush.
    s.pace(m_options.fpsCap);
    s.frame = static_cast<std::uint32_t>(context.frameIndex());
    s.simulatedSeconds += deltaSeconds;
    s.ui->setTime(s.simulatedSeconds);
    s.runInputSteps(context);

    // Devices -> action map -> input script -> UI commands.
    input::ActionState actions = s.input.resolve(s.collector.endFrame());
    if (s.script) {
        s.script->apply(context.frameIndex(), actions, s.input.settings().pressThreshold);
    }
    s.commands.clear();
    s.nav.update(actions, deltaSeconds, s.commands);
    s.report.commandsDropped += s.commands.dropped();

    // The measured UI pass, CPU side: the commands, then the context's update.
    AllocationCounter::begin();
    const Clock::time_point inputStart = Clock::now();
    if (s.screen->visible()) {
        s.screen->apply(s.commands);
    } else {
        s.whileClosed(context);
    }
    s.inputMs = millisecondsSince(inputStart);
    s.drawable = context.framebufferSize();
    s.displayScale = context.window().displayScale();
    s.view->resize(s.drawable, s.displayScale);
    s.view->update();
    const std::size_t focusUpdated = s.report.focusChanges.size();
    const std::uint64_t cardsUpdated = s.screen->cardRefreshes();
    const std::uint64_t bindsUpdated = s.screen->windowBinds();
    const std::uint64_t rowBindsUpdated = s.screen->rowBinds();
    const Clock::time_point afterStart = Clock::now();
    s.screen->afterUpdate();
    s.afterUpdateMs = millisecondsSince(afterStart);
    AllocationCounter::end();
    s.updateAllocations = s.takeAllocations();
    // Which frame laid out a change: this one for the changes made before its update (events, commands), the next one
    // for those made after it (the card settling, the window following the layout).
    s.focusMovedThisFrame = s.focusMovedLate || focusUpdated > s.focusChangesSeen;
    s.cardLaidOutThisFrame = s.cardFilledLate || cardsUpdated > s.cardRefreshesSeen;
    s.rowsBoundThisFrame = s.rowsBoundLate || bindsUpdated > s.windowBindsSeen;
    // The rows bound before this update are laid out in it; those bound after it count for the next frame.
    s.rowBindsThisFrame = static_cast<std::uint32_t>(rowBindsUpdated - s.rowBindsSeen);
    s.rowBindsSeen = rowBindsUpdated;
    s.focusChangesSeen = s.report.focusChanges.size();
    s.cardRefreshesSeen = s.screen->cardRefreshes();
    s.windowBindsSeen = s.screen->windowBinds();
    s.focusMovedLate = s.focusChangesSeen > focusUpdated;
    s.cardFilledLate = s.cardRefreshesSeen > cardsUpdated;
    s.rowsBoundLate = s.windowBindsSeen > bindsUpdated;
    // The selection's moves (a scroll keeps the focus on its row: the moves are counted, not the focus changes).
    const std::optional<std::size_t> selection = s.model->selectedPosition();
    s.selectionMovedThisFrame = selection != s.selectionSeen;
    if (s.selectionMovedThisFrame) {
        s.selectionSeen = selection;
        if (selection && s.report.selections.size() < s.report.selections.capacity()) {
            s.report.selections.push_back(SelectionRecord{s.frame, *selection});
        }
    }
    if (s.displayScale != s.lastDisplayScale) {
        spdlog::warn("Frame {}: the display scale changed under the run, {:.2f} -> {:.2f}", s.frame, s.lastDisplayScale,
                     s.displayScale);
        s.report.displayScaleChanges.emplace_back(s.frame, s.displayScale);
        s.lastDisplayScale = s.displayScale;
    }

    s.logRecords();
    s.runChecks();
    if constexpr (kWatchDocuments) {
        s.screen->reloadIfChanged();
    }
    s.graphics->pollShaderReload();
}

void SpikeApp::onRender(app::AppContext& context) {
    if (!m_state) {
        return;
    }
    State& s = *m_state;
    gl::RenderView view = app::makeRenderView(*s.scene, s.camera);
    view.post.antialiasing = true;
    view.timeSeconds = static_cast<float>(s.simulatedSeconds);

    // A screenshot of this frame: SceneGraphics renders it (scene, then the UI through the screenshot overlay) and
    // writes it under <user data>/screenshots, from where it moves to the path asked for.
    const ScreenshotRequest* capture = s.captureFor(m_options.screenshots);
    std::filesystem::path screenshots;
    std::vector<std::filesystem::path> before;
    if (capture != nullptr) {
        screenshots = context.paths().screenshotsDir();
        before = pngFiles(screenshots);
    }
    s.graphics->renderFrame(context, view, capture != nullptr);
    if (capture != nullptr) {
        std::vector<std::filesystem::path> written;
        for (const std::filesystem::path& file : pngFiles(screenshots)) {
            if (!std::binary_search(before.begin(), before.end(), file)) {
                written.push_back(file);
            }
        }
        const bool moved = written.size() == 1 && moveFile(written.front(), capture->path);
        if (moved) {
            spdlog::info("Frame {}: screenshot {}", s.frame, core::pathToGenericUtf8(capture->path));
        } else {
            spdlog::error("Frame {}: screenshot {} not written", s.frame, core::pathToGenericUtf8(capture->path));
        }
        s.report.screenshots.push_back(ScreenshotRecord{s.frame, core::pathToGenericUtf8(capture->path), moved});
        ++s.nextScreenshot;
    }
    s.recordSample(context);
}

void SpikeApp::onShutdown(app::AppContext& context) {
    if (!m_state) {
        return;
    }
    State& s = *m_state;
    s.graphics->finishRun(context);
    ReportData& report = s.report;
    report.framesRendered = context.frameIndex();
    report.checksNotRun = m_options.scenario ? s.checksNotRun() : 0;
    report.windowRows = s.model->windowRows();
    report.finalSelection = s.model->selectedPosition();
    report.finalSelectionId = s.model->selected() != nullptr ? s.model->selected()->id : std::string();
    const ui::UiLogCounts log = s.ui->logCounts();
    report.rmlErrors = log.errors;
    report.rmlWarnings = log.warnings;
    for (const std::string& line : timingSummaryLines(report)) {
        spdlog::info("{}", line);
    }
    spdlog::info(
        "Final selection: position {} ({}) of {} game(s), {} row(s) shown", positionText(report.finalSelection),
        report.finalSelectionId.empty() ? "none" : report.finalSelectionId, s.model->count(), report.windowRows);
    if (m_options.expectSelection) {
        const bool reached = report.finalSelection == *m_options.expectSelection;
        report.checks.push_back(CheckRecord{s.frame, fmt::format("expect-selection {}", *m_options.expectSelection),
                                            reached, "the selection ended at " + positionText(report.finalSelection),
                                            false});
    }
    const auto failed = static_cast<std::size_t>(std::count_if(
        report.checks.begin(), report.checks.end(), [](const CheckRecord& c) { return !c.passed && !c.skipped; }));
    const auto skipped = static_cast<std::size_t>(
        std::count_if(report.checks.begin(), report.checks.end(), [](const CheckRecord& c) { return c.skipped; }));
    const bool screenshotsWritten =
        std::all_of(report.screenshots.begin(), report.screenshots.end(), [](const ScreenshotRecord& r) {
            return r.written;
        });
    spdlog::info("spike_ui: {} check(s), {} failed, {} skipped, {} never ran; {} focus change(s), {} move(s) of the "
                 "selection; RmlUi log: {} error(s), {} warning(s)",
                 report.checks.size(), failed, skipped, report.checksNotRun, report.focusChanges.size(),
                 report.selections.size(), log.errors, log.warnings);
    if (m_options.report) {
        if (auto written = writeReport(*m_options.report, report); written) {
            spdlog::info("Report written: {}", core::pathToGenericUtf8(*m_options.report));
        } else {
            context.failRun(Error{ErrorCode::IoError, "the report could not be written: " + written.error().message,
                                  core::pathToUtf8(*m_options.report)});
        }
    }
    if (failed > 0) {
        context.failRun(Error{ErrorCode::ValidationFailed, fmt::format("{} check(s) failed", failed), {}});
    } else if (report.checksNotRun > 0) {
        // Quit early (Back with the catalog closed, the window closed) or too few --frames: an unchecked run is no pass.
        context.failRun(Error{ErrorCode::ValidationFailed,
                              fmt::format("{} scenario check(s) never ran: the run ended after {} frame(s), the "
                                          "scenario needs {}",
                                          report.checksNotRun, report.framesRendered, s.scenario.frames),
                              {}});
    } else if (log.errors > 0 || log.warnings > 0) {
        context.failRun(Error{ErrorCode::ValidationFailed,
                              fmt::format("RmlUi logged {} error(s) and {} warning(s)", log.errors, log.warnings), {}});
    } else if (!screenshotsWritten) {
        context.failRun(Error{ErrorCode::IoError, "a --screenshot-at was not written", {}});
    }
    // UI first (screen, model, source, view, system), then the scene's GPU resources, while the context is current.
    s.screen.reset();
    s.model.reset();
    s.source.reset();
    s.view.reset();
    s.ui.reset();
    s.pad.reset();
    s.scene.reset();
    s.graphics.reset();
    m_state.reset();
}

} // namespace rv::spike_ui
