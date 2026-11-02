#include <rv/ui/emulators_model.hpp>

#include <algorithm>
#include <utility>

#include <fmt/format.h>

namespace rv::ui {
namespace {

constexpr std::array<EmulatorsButton, 6> kButtons = {EmulatorsButton::Sample,       EmulatorsButton::Test,
                                                     EmulatorsButton::Program,      EmulatorsButton::ClearProgram,
                                                     EmulatorsButton::Save,         EmulatorsButton::Cancel};

constexpr std::string_view kReady = "ready";
constexpr std::string_view kEmulatorNotFound = "emulator-not-found";
constexpr std::string_view kVerified = "verified";

} // namespace

std::string_view toString(EmulatorsState state) noexcept {
    switch (state) {
    case EmulatorsState::Loading:
        return "loading";
    case EmulatorsState::Listed:
        return "listed";
    case EmulatorsState::Unavailable:
        return "unavailable";
    }
    return "unknown";
}

std::string_view toString(EmulatorsView view) noexcept {
    switch (view) {
    case EmulatorsView::Systems:
        return "systems";
    case EmulatorsView::System:
        return "system";
    case EmulatorsView::Choice:
        return "choice";
    case EmulatorsView::Testing:
        return "testing";
    }
    return "unknown";
}

std::string_view toString(EmulatorsButton button) noexcept {
    switch (button) {
    case EmulatorsButton::Sample:
        return "sample";
    case EmulatorsButton::Test:
        return "test";
    case EmulatorsButton::Program:
        return "program";
    case EmulatorsButton::ClearProgram:
        return "clear-program";
    case EmulatorsButton::Save:
        return "save";
    case EmulatorsButton::Cancel:
        return "cancel";
    }
    return "unknown";
}

std::optional<EmulatorsButton> emulatorsButtonFromName(std::string_view name) noexcept {
    for (const EmulatorsButton button : kButtons) {
        if (toString(button) == name) {
            return button;
        }
    }
    return std::nullopt;
}

std::string_view elementIdOf(EmulatorsButton button) noexcept {
    switch (button) {
    case EmulatorsButton::Sample:
        return "em-sample";
    case EmulatorsButton::Test:
        return "em-test";
    case EmulatorsButton::Program:
        return "em-program";
    case EmulatorsButton::ClearProgram:
        return "em-clear-program";
    case EmulatorsButton::Save:
        return "em-save";
    case EmulatorsButton::Cancel:
        return "em-cancel";
    }
    return {};
}

std::string_view toString(EmulatorsCommandKind kind) noexcept {
    switch (kind) {
    case EmulatorsCommandKind::Close:
        return "close";
    case EmulatorsCommandKind::Load:
        return "load";
    case EmulatorsCommandKind::Samples:
        return "samples";
    case EmulatorsCommandKind::Test:
        return "test";
    case EmulatorsCommandKind::Save:
        return "save";
    case EmulatorsCommandKind::Clear:
        return "clear";
    case EmulatorsCommandKind::Browse:
        return "browse";
    case EmulatorsCommandKind::ClearProgram:
        return "clear-program";
    }
    return "unknown";
}

void EmulatorsModel::open() {
    m_open = true;
    m_gameSystem.clear();
    m_game.clear();
    m_gameTitle.clear();
    m_own.reset();
    m_view = EmulatorsView::Systems;
    m_state = EmulatorsState::Loading;
    m_verdict.clear();
    m_problem.clear();
    m_saved = false;
    m_request = EmulatorsCommand{EmulatorsCommandKind::Load};
    ++m_revision;
}

void EmulatorsModel::openForGame(std::string system, std::string gameId, std::string title,
                                 std::optional<std::pair<bool, std::string>> own) {
    open();
    m_gameSystem = std::move(system);
    m_game = std::move(gameId);
    m_gameTitle = std::move(title);
    m_own = std::move(own);
    m_chosen.reset();
}

void EmulatorsModel::close() noexcept {
    if (m_open) {
        m_open = false;
        ++m_revision;
    }
}

void EmulatorsModel::prepare() {
    m_request = EmulatorsCommand{EmulatorsCommandKind::Load};
}

std::optional<EmulatorsCommand> EmulatorsModel::takeRequest() noexcept {
    return std::exchange(m_request, std::nullopt);
}

const EmulatorSystem* EmulatorsModel::system() const noexcept {
    return m_system < m_systems.size() ? &m_systems[m_system] : nullptr;
}

const EmulatorOption* EmulatorsModel::option() const noexcept {
    return m_chosen ? &*m_chosen : nullptr;
}

bool EmulatorsModel::offersClear() const noexcept {
    const EmulatorSystem* shown = system();
    return shown != nullptr && (forGame() ? m_own.has_value() : shown->stored);
}

std::size_t EmulatorsModel::optionRows() const noexcept {
    const EmulatorSystem* shown = system();
    return shown == nullptr ? 0 : shown->options.size() + (offersClear() ? 1 : 0);
}

std::vector<EmulatorsButton> EmulatorsModel::buttons() const {
    std::vector<EmulatorsButton> shown;
    const EmulatorOption* chosen = option();
    if (chosen == nullptr) {
        return {EmulatorsButton::Cancel};
    }
    const bool ready = chosen->readiness == kReady;
    if (ready && !m_samples.empty()) {
        shown.push_back(EmulatorsButton::Sample);
        shown.push_back(EmulatorsButton::Test);
    }
    if (!chosen->emulator.empty() && chosen->readiness == kEmulatorNotFound) {
        shown.push_back(EmulatorsButton::Program);
    }
    if (chosen->programChosen) {
        shown.push_back(EmulatorsButton::ClearProgram);
    }
    shown.push_back(EmulatorsButton::Save);
    shown.push_back(EmulatorsButton::Cancel);
    return shown;
}

std::size_t EmulatorsModel::readySystems() const noexcept {
    return static_cast<std::size_t>(std::count_if(m_systems.begin(), m_systems.end(), [](const EmulatorSystem& shown) {
        return shown.readiness == kReady;
    }));
}

std::size_t EmulatorsModel::blockedSystems() const noexcept {
    return m_systems.size() - readySystems();
}

std::string EmulatorsModel::systemElement(std::size_t index) {
    return fmt::format("em-system-{}", index);
}

std::string EmulatorsModel::optionElement(std::size_t index) {
    return fmt::format("em-option-{}", index);
}

std::string EmulatorsModel::focusedElement() const {
    switch (m_view) {
    case EmulatorsView::Systems:
        return m_systems.empty() ? std::string() : systemElement(m_system);
    case EmulatorsView::System:
        return optionRows() == 0 ? std::string() : optionElement(m_option);
    case EmulatorsView::Choice:
        return std::string(elementIdOf(m_button));
    case EmulatorsView::Testing:
        return {};
    }
    return {};
}

void EmulatorsModel::show(EmulatorsView view) {
    m_view = view;
    ++m_revision;
}

void EmulatorsModel::settleButton() {
    const std::vector<EmulatorsButton> shown = buttons();
    if (std::find(shown.begin(), shown.end(), m_button) == shown.end()) {
        m_button = shown.front();
    }
}

void EmulatorsModel::setTable(std::vector<EmulatorSystem> systems) {
    std::string focused;
    if (const EmulatorSystem* shown = system()) {
        focused = shown->name;
    }
    // A list holds no options: those of the system shown stay until its own answer, asked again.
    for (EmulatorSystem& row : systems) {
        const auto known = std::find_if(m_systems.begin(), m_systems.end(),
                                        [&](const EmulatorSystem& old) { return old.name == row.name; });
        if (row.options.empty() && known != m_systems.end()) {
            row.options = std::move(known->options);
        }
    }
    m_systems = std::move(systems);
    m_state = EmulatorsState::Listed;
    const auto again = std::find_if(m_systems.begin(), m_systems.end(),
                                    [&](const EmulatorSystem& shown) { return shown.name == focused; });
    m_system = again == m_systems.end() ? 0 : static_cast<std::size_t>(again - m_systems.begin());
    if (forGame() && m_view == EmulatorsView::Systems) {
        const auto title = std::find_if(m_systems.begin(), m_systems.end(),
                                        [&](const EmulatorSystem& shown) { return shown.name == m_gameSystem; });
        if (title == m_systems.end()) {
            m_state = EmulatorsState::Unavailable; // its system has no games any more
            ++m_revision;
            return;
        }
        m_system = static_cast<std::size_t>(title - m_systems.begin());
        m_request = enter();
        ++m_revision;
        return;
    }
    const std::size_t rows = optionRows();
    m_option = rows == 0 ? 0 : std::min(m_option, rows - 1);
    syncChosen();
    if (m_view != EmulatorsView::Systems && system() == nullptr) {
        m_view = EmulatorsView::Systems; // the system went away
        m_chosen.reset();
    } else if (m_view == EmulatorsView::Choice || m_view == EmulatorsView::System) {
        m_request = commandOf(EmulatorsCommandKind::Samples);
    }
    ++m_revision;
}

void EmulatorsModel::syncChosen() {
    if (m_chosen && system() != nullptr) {
        const auto& options = system()->options;
        const auto same = std::find_if(options.begin(), options.end(), [&](const EmulatorOption& option) {
            return option.profile == m_chosen->profile && option.value == m_chosen->value;
        });
        if (same != options.end()) {
            m_chosen = *same;
        }
    }
    if (m_view == EmulatorsView::Choice) {
        settleButton();
    }
}

void EmulatorsModel::setOptions(const std::string& name, std::vector<EmulatorOption> options) {
    const auto found = std::find_if(m_systems.begin(), m_systems.end(),
                                    [&](const EmulatorSystem& shown) { return shown.name == name; });
    if (found == m_systems.end()) {
        return;
    }
    found->options = std::move(options);
    markOwn();
    const std::size_t rows = optionRows();
    m_option = rows == 0 ? 0 : std::min(m_option, rows - 1);
    syncChosen();
    ++m_revision;
}

void EmulatorsModel::markOwn() {
    EmulatorSystem* shown = m_system < m_systems.size() ? &m_systems[m_system] : nullptr;
    if (!forGame() || !m_own || shown == nullptr) {
        return;
    }
    for (EmulatorOption& option : shown->options) {
        option.chosen = option.profile == m_own->first && option.value == m_own->second;
    }
}

void EmulatorsModel::setUnavailable() {
    m_systems.clear();
    m_state = EmulatorsState::Unavailable;
    m_view = EmulatorsView::Systems;
    m_chosen.reset();
    ++m_revision;
}

void EmulatorsModel::setSamples(const std::string& system, std::vector<EmulatorSample> samples) {
    const EmulatorSystem* shown = this->system();
    if (shown == nullptr || shown->name != system) {
        return; // the user went elsewhere
    }
    if (forGame()) {
        samples = {EmulatorSample{m_game, m_gameTitle}}; // the title's test runs the title
    }
    if (samples != m_samples) {
        m_sample = 0; // asked again after a change, the same list keeps the game shown
    }
    m_samples = std::move(samples);
    m_samplesKnown = true;
    if (m_view == EmulatorsView::Choice) {
        settleButton();
    }
    ++m_revision;
}

void EmulatorsModel::testStarted() {
    m_verdict = "running";
    m_problem.clear();
    show(EmulatorsView::Testing);
}

void EmulatorsModel::testEnded(std::string_view verdict) {
    m_verdict = std::string(verdict);
    if (verdict == kVerified) {
        if (forGame() && m_chosen) {
            m_own = std::pair(m_chosen->profile, m_chosen->value);
        }
        m_chosen.reset();
        m_saved = false;
        m_request = EmulatorsCommand{EmulatorsCommandKind::Load};
        show(EmulatorsView::System);
        return;
    }
    if (m_chosen) {
        m_view = EmulatorsView::Choice;
        settleButton();
    }
    ++m_revision;
}

void EmulatorsModel::changeEnded(std::string_view problem, bool toSystem) {
    m_problem = std::string(problem);
    if (!problem.empty()) {
        ++m_revision;
        return;
    }
    m_request = EmulatorsCommand{EmulatorsCommandKind::Load};
    if (toSystem) {
        if (forGame()) {
            // Saved with a choice on show, cleared from the list.
            m_own = m_chosen ? std::optional(std::pair(m_chosen->profile, m_chosen->value)) : std::nullopt;
            markOwn();
        }
        m_chosen.reset();
        show(EmulatorsView::System);
        return;
    }
    ++m_revision;
}

void EmulatorsModel::moveRow(std::ptrdiff_t delta) {
    std::size_t& focus = m_view == EmulatorsView::Systems ? m_system : m_option;
    const std::size_t count = m_view == EmulatorsView::Systems ? m_systems.size() : optionRows();
    if (count == 0) {
        return;
    }
    const std::size_t next = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(
        static_cast<std::ptrdiff_t>(focus) + delta, 0, static_cast<std::ptrdiff_t>(count) - 1));
    if (next != focus) {
        focus = next;
        ++m_revision;
    }
}

void EmulatorsModel::stepSample(int direction) {
    if (m_samples.size() < 2) {
        return;
    }
    const std::size_t count = m_samples.size();
    m_sample = direction < 0 ? (m_sample + count - 1) % count : (m_sample + 1) % count;
    ++m_revision;
}

EmulatorsCommand EmulatorsModel::commandOf(EmulatorsCommandKind kind) const {
    EmulatorsCommand command;
    command.kind = kind;
    command.forGame = forGame();
    if (const EmulatorSystem* shown = system()) {
        command.system = shown->name;
    }
    if (const EmulatorOption* chosen = option()) {
        command.profile = chosen->profile;
        command.value = chosen->value;
        command.emulator = chosen->emulator;
    }
    if (m_sample < m_samples.size()) {
        command.game = m_samples[m_sample].gameId;
        command.gameTitle = m_samples[m_sample].title;
    }
    return command;
}

void EmulatorsModel::chooseOption(std::size_t index) {
    const EmulatorSystem* shown = system();
    if (shown == nullptr || index >= shown->options.size()) {
        return;
    }
    m_option = index;
    m_chosen = shown->options[index];
    m_verdict.clear();
    m_problem.clear();
    const std::vector<EmulatorsButton> offered = buttons();
    // The way forward: the test when it can run, else what repairs it; the sample is one step above.
    m_button = std::find(offered.begin(), offered.end(), EmulatorsButton::Test) != offered.end() ? EmulatorsButton::Test
                                                                                                  : offered.front();
    show(EmulatorsView::Choice);
}

std::optional<EmulatorsCommand> EmulatorsModel::enter() {
    const EmulatorSystem* shown = system();
    if (shown == nullptr) {
        return std::nullopt;
    }
    m_option = 0;
    m_chosen.reset();
    m_samples.clear();
    m_sample = 0;
    m_samplesKnown = false;
    m_verdict.clear();
    m_problem.clear();
    m_saved = false;
    show(EmulatorsView::System);
    return commandOf(EmulatorsCommandKind::Samples);
}

std::optional<EmulatorsCommand> EmulatorsModel::press(EmulatorsButton button) {
    switch (button) {
    case EmulatorsButton::Sample:
        stepSample(1);
        return std::nullopt;
    case EmulatorsButton::Test:
        return commandOf(EmulatorsCommandKind::Test);
    case EmulatorsButton::Program:
        return commandOf(EmulatorsCommandKind::Browse);
    case EmulatorsButton::ClearProgram:
        return commandOf(EmulatorsCommandKind::ClearProgram);
    case EmulatorsButton::Save:
        m_saved = true;
        return commandOf(EmulatorsCommandKind::Save);
    case EmulatorsButton::Cancel:
        m_chosen.reset();
        m_verdict.clear();
        m_problem.clear();
        show(EmulatorsView::System);
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<EmulatorsCommand> EmulatorsModel::apply(UiCommand command) {
    if (!m_open) {
        return std::nullopt;
    }
    if (command == UiCommand::Back) {
        switch (m_view) {
        case EmulatorsView::Systems: // for one title: the table has not come
            close();
            return EmulatorsCommand{EmulatorsCommandKind::Close};
        case EmulatorsView::System:
            if (forGame()) {
                close();
                return EmulatorsCommand{EmulatorsCommandKind::Close};
            }
            m_verdict.clear();
            m_problem.clear();
            show(EmulatorsView::Systems);
            return std::nullopt;
        case EmulatorsView::Choice:
            return press(EmulatorsButton::Cancel);
        case EmulatorsView::Testing:
            return std::nullopt; // a game is on its way: the game-session panel answers
        }
    }
    switch (m_view) {
    case EmulatorsView::Systems:
    case EmulatorsView::System:
        switch (command) {
        case UiCommand::Up:
            moveRow(-1);
            return std::nullopt;
        case UiCommand::Down:
            moveRow(1);
            return std::nullopt;
        case UiCommand::PageUp:
            moveRow(-static_cast<std::ptrdiff_t>(emulators::kPageRows));
            return std::nullopt;
        case UiCommand::PageDown:
            moveRow(static_cast<std::ptrdiff_t>(emulators::kPageRows));
            return std::nullopt;
        case UiCommand::Home:
            moveRow(-static_cast<std::ptrdiff_t>(m_systems.size() + optionRows()));
            return std::nullopt;
        case UiCommand::End:
            moveRow(static_cast<std::ptrdiff_t>(m_systems.size() + optionRows()));
            return std::nullopt;
        case UiCommand::Activate:
            if (m_view == EmulatorsView::Systems) {
                return enter();
            }
            if (offersClear() && m_option + 1 == optionRows()) {
                return commandOf(EmulatorsCommandKind::Clear);
            }
            chooseOption(m_option);
            return std::nullopt;
        default:
            return std::nullopt;
        }
    case EmulatorsView::Choice: {
        const std::vector<EmulatorsButton> shown = buttons();
        const auto at = std::find(shown.begin(), shown.end(), m_button);
        switch (command) {
        case UiCommand::Up:
            if (at != shown.begin() && at != shown.end()) {
                m_button = *(at - 1);
                ++m_revision;
            }
            return std::nullopt;
        case UiCommand::Down:
            if (at != shown.end() && at + 1 != shown.end()) {
                m_button = *(at + 1);
                ++m_revision;
            }
            return std::nullopt;
        case UiCommand::Left:
        case UiCommand::Right:
            if (m_button == EmulatorsButton::Sample) {
                stepSample(command == UiCommand::Left ? -1 : 1);
            }
            return std::nullopt;
        case UiCommand::Activate:
            return at != shown.end() ? press(m_button) : std::nullopt;
        default:
            return std::nullopt;
        }
    }
    case EmulatorsView::Testing:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<EmulatorsCommand> EmulatorsModel::click(const EmulatorsClick& click) {
    if (!m_open) {
        return std::nullopt;
    }
    switch (click.kind) {
    case EmulatorsClick::Kind::System:
        if (m_view != EmulatorsView::Systems || click.index < 0 ||
            static_cast<std::size_t>(click.index) >= m_systems.size()) {
            return std::nullopt;
        }
        m_system = static_cast<std::size_t>(click.index);
        return enter();
    case EmulatorsClick::Kind::Option:
        if (m_view == EmulatorsView::System && click.index >= 0) {
            chooseOption(static_cast<std::size_t>(click.index));
        }
        return std::nullopt;
    case EmulatorsClick::Kind::Clear:
        if (m_view == EmulatorsView::System && offersClear()) {
            m_option = optionRows() - 1;
            return commandOf(EmulatorsCommandKind::Clear);
        }
        return std::nullopt;
    case EmulatorsClick::Kind::Button: {
        if (m_view != EmulatorsView::Choice) {
            return std::nullopt;
        }
        const std::vector<EmulatorsButton> shown = buttons();
        if (std::find(shown.begin(), shown.end(), click.button) == shown.end()) {
            return std::nullopt;
        }
        m_button = click.button;
        ++m_revision;
        return press(click.button);
    }
    case EmulatorsClick::Kind::Sample:
        if (m_view == EmulatorsView::Choice) {
            m_button = EmulatorsButton::Sample;
            stepSample(click.index);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace rv::ui
