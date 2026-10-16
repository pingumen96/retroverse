#include "spike_app.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/app/command_line.hpp>
#include <rv/app/scene_graphics.hpp>
#include <rv/app/scene_instance.hpp>
#include <rv/core/frame_clock.hpp>
#include <rv/core/math.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/version.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/platform/graphics_window.hpp>
#include <rv/platform/sdl_input_collector.hpp>
#include <rv/render/framebuffer.hpp>
#include <rv/render/gl_device.hpp>
#include <rv/render/scene_renderer.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>
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

[[nodiscard]] const char* yesNo(bool value) noexcept {
    return value ? "yes" : "no";
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
    // Declared in the order they are created; destroyed in reverse: the screen before its model and view, the view
    // before the UI system, the UI before the scene's GPU resources.
    std::unique_ptr<app::SceneGraphics> graphics;
    std::optional<app::SceneInstance> scene;
    scene::Camera camera;
    std::unique_ptr<ui::UiSystem> ui;
    std::unique_ptr<ui::UiView> view;
    std::unique_ptr<ui::QuickCatalogModel> model;
    std::unique_ptr<ui::QuickCatalogScreen> screen;

    platform::SdlInputCollector collector;
    ui::NavInput nav;
    ui::UiCommandList commands;
    std::optional<VirtualPad> pad;
    bool padFailureLogged = false;
    Scenario scenario;
    std::size_t nextInputStep = 0;
    std::size_t nextCheckStep = 0;
    std::size_t nextScreenshot = 0;
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
    /// The screen's work after the context's update (the card settling, the chips' fades, a lost focus), counted with
    /// the update.
    double afterUpdateMs = 0.0;
    AllocationCounts updateAllocations;
    /// --fps-cap: when the next frame may start (SDL_GetTicksNS), 0 before the first.
    Uint64 nextFrameNs = 0;
    std::optional<double> lastGpuMs;
    std::size_t focusLogged = 0;
    std::size_t actionsLogged = 0;
    /// Whether this frame's update and render handled a change of the UI. A focus move or a card filled before the
    /// context's update is laid out and drawn in the same frame; one made after it (the card settling in the screen's
    /// afterUpdate) in the next frame, which then counts as the changed one.
    bool focusMovedThisFrame = false;
    bool cardLaidOutThisFrame = false;
    bool focusMovedLate = false;
    bool cardFilledLate = false;
    /// The focus changes and card refreshes made up to the end of the previous frame's update.
    std::size_t focusChangesSeen = 0;
    std::uint64_t cardRefreshesSeen = 0;
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
            default:
                break;
            }
            if (!done && !padFailureLogged) {
                spdlog::error("Frame {}: scenario step '{}' failed: {}", frame, toString(step.kind), SDL_GetError());
                padFailureLogged = true;
            }
        }
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
        // The focused game visible in the list (scrolled into view).
        if (layout.focused.starts_with(qc::kGamePrefix)) {
            const std::optional<glm::vec4> list = boxOf("game-list");
            const std::optional<glm::vec4> focused = view->elementBox(layout.focused);
            const bool visible = list && focused && focused->y >= list->y - kTolerance &&
                                 focused->y + focused->w <= list->y + list->w + kTolerance;
            check("layout " + label + ": focused game visible in the list", visible, layout.focused);
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
        check("same-layout " + label + " = " + reference, differences.empty(),
              differences.empty() ? fmt::format("{} boxes equal at {}x{}", before->boxes.size(), now->size.x,
                                                now->size.y)
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
        detail += fmt::format("; layout theme {} -> {}", before->compact ? "compact" : "regular",
                              now->compact ? "compact" : "regular");
        check(name, larger, detail);
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
            const auto changes = static_cast<std::uint32_t>(
                std::count_if(report.focusChanges.begin(), report.focusChanges.end(),
                              [&step](const FocusRecord& record) { return record.frame >= step.since; }));
            check(named(step, fmt::format("steady since frame {}", step.since)), changes == step.count,
                  fmt::format("{} focus change(s), {} expected", changes, step.count));
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
        case StepKind::ExpectVisible: {
            const std::size_t visible = model->visible().size();
            check(named(step, fmt::format("visible {}", step.count)), visible == step.count,
                  fmt::format("{} game(s) listed", visible));
            break;
        }
        case StepKind::ExpectCard: {
            // The title the document shows, against the game the actions act on.
            const ui::CatalogEntry* selected = model->selected();
            const std::string expected = selected != nullptr ? selected->title : std::string("(no game)");
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
    s.runTextScale = m_options.textScale;
    s.runHighContrast = m_options.highContrast;
    s.textScale = m_options.textScale;
    s.view->setTextScale(m_options.textScale);
    s.model = std::make_unique<ui::QuickCatalogModel>(mockCatalog());
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

    // Input: every connected pad by hand (--interactive); the scripted virtual pad alone in scenario runs, so that a pad
    // in use on the machine cannot move the focus under the checks.
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
        s.scenario = defaultScenario();
        if (!frameLimit || *frameLimit < s.scenario.frames) {
            spdlog::warn("The scenario needs {} frames; with {} the checks after the last frame never run and the run "
                         "fails",
                         s.scenario.frames, frameLimit ? std::to_string(*frameLimit) : std::string("no limit"));
        }
    } else {
        s.collector.gamepads().openConnected();
    }
    s.report.samples.reserve(frameLimit ? *frameLimit : kInteractiveSampleCapacity);
    s.report.focusChanges.reserve(frameLimit ? *frameLimit / 2 + 64 : 4096);
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
    report.fixedDeltaSeconds = m_options.run.fixedDeltaSeconds;
    report.vsync = m_options.run.vsync;
    report.fpsCap = m_options.fpsCap;
    report.fonts = s.ui->loadedFontFaces();
    report.phases = s.scenario.phases;
    spdlog::info("spike_ui: {}x{} pixels, display scale {:.2f}, {} scripted step(s), {} screenshot(s) asked",
                 report.initialSize.x, report.initialSize.y, report.displayScale, s.scenario.steps.size(),
                 m_options.screenshots.size());
    s.logRecords(); // the first focus
    s.focusChangesSeen = report.focusChanges.size(); // laid out by the screen's creation
    s.cardRefreshesSeen = s.screen->cardRefreshes();
    m_state = std::move(state);
    return ok();
}

void SpikeApp::onEvent(app::AppContext& /*context*/, const SDL_Event& event) {
    if (!m_state) {
        return;
    }
    State& s = *m_state;
    if (s.pad) {
        if (const std::optional<SDL_JoystickID> pad = gamepadOf(event); pad && *pad != s.pad->id()) {
            return; // a real pad: scripted runs hear the virtual one only
        }
    }
    // The UI first (mouse, text, keys that are not commands); what it does not use goes to the input collector, which
    // turns pads and command keys into the raw frame NavInput reads.
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

    const input::RawInputFrame raw = s.collector.endFrame();
    s.commands.clear();
    s.nav.update(raw, deltaSeconds, s.commands);
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
    const Clock::time_point afterStart = Clock::now();
    s.screen->afterUpdate();
    s.afterUpdateMs = millisecondsSince(afterStart);
    AllocationCounter::end();
    s.updateAllocations = s.takeAllocations();
    // Which frame laid out a change: this one for the changes made before its update (events, commands), the next one
    // for those made after it (the card settling).
    s.focusMovedThisFrame = s.focusMovedLate || focusUpdated > s.focusChangesSeen;
    s.cardLaidOutThisFrame = s.cardFilledLate || cardsUpdated > s.cardRefreshesSeen;
    s.focusChangesSeen = s.report.focusChanges.size();
    s.cardRefreshesSeen = s.screen->cardRefreshes();
    s.focusMovedLate = s.focusChangesSeen > focusUpdated;
    s.cardFilledLate = s.cardRefreshesSeen > cardsUpdated;
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
    const ui::UiLogCounts log = s.ui->logCounts();
    report.rmlErrors = log.errors;
    report.rmlWarnings = log.warnings;
    for (const std::string& line : timingSummaryLines(report)) {
        spdlog::info("{}", line);
    }
    const auto failed = static_cast<std::size_t>(std::count_if(
        report.checks.begin(), report.checks.end(), [](const CheckRecord& c) { return !c.passed && !c.skipped; }));
    const auto skipped = static_cast<std::size_t>(
        std::count_if(report.checks.begin(), report.checks.end(), [](const CheckRecord& c) { return c.skipped; }));
    const bool screenshotsWritten =
        std::all_of(report.screenshots.begin(), report.screenshots.end(), [](const ScreenshotRecord& r) {
            return r.written;
        });
    spdlog::info("spike_ui: {} check(s), {} failed, {} skipped, {} never ran; {} focus change(s); RmlUi log: {} "
                 "error(s), {} warning(s)",
                 report.checks.size(), failed, skipped, report.checksNotRun, report.focusChanges.size(), log.errors,
                 log.warnings);
    if (m_options.report) {
        if (auto written = writeReport(*m_options.report, report); written) {
            spdlog::info("Report written: {}", core::pathToGenericUtf8(*m_options.report));
        } else {
            context.failRun(Error{ErrorCode::IoError, "the report could not be written: " + written.error().message,
                                  core::pathToUtf8(*m_options.report)});
        }
    }
    if (failed > 0) {
        context.failRun(Error{ErrorCode::ValidationFailed, fmt::format("{} scenario check(s) failed", failed), {}});
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
    // UI first (screen, model, view, system), then the scene's GPU resources, while the context is current.
    s.screen.reset();
    s.model.reset();
    s.view.reset();
    s.ui.reset();
    s.pad.reset();
    s.scene.reset();
    s.graphics.reset();
    m_state.reset();
}

} // namespace rv::spike_ui
