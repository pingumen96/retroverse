#include <rv/ui/wizard_model.hpp>

#include <algorithm>
#include <utility>

namespace rv::ui {
namespace {

constexpr std::array<WizardButton, 11> kButtons = {
    WizardButton::Next,          WizardButton::Previous,  WizardButton::Quit,         WizardButton::Retry,
    WizardButton::Enter,         WizardButton::FolderHome, WizardButton::FolderInstall, WizardButton::FolderRom,
    WizardButton::FolderMedia,   WizardButton::Lighting,   WizardButton::ReduceMotion};

} // namespace

std::string_view toString(WizardPage page) noexcept {
    switch (page) {
    case WizardPage::Welcome:
        return "welcome";
    case WizardPage::Detection:
        return "detection";
    case WizardPage::Preview:
        return "preview";
    case WizardPage::Import:
        return "import";
    case WizardPage::Emulators:
        return "emulators";
    case WizardPage::Appearance:
        return "appearance";
    case WizardPage::World:
        return "world";
    }
    return "unknown";
}

std::string_view toString(WizardButton button) noexcept {
    switch (button) {
    case WizardButton::Next:
        return "next";
    case WizardButton::Previous:
        return "previous";
    case WizardButton::Quit:
        return "quit";
    case WizardButton::Retry:
        return "retry";
    case WizardButton::Enter:
        return "enter";
    case WizardButton::FolderHome:
        return "folder-home";
    case WizardButton::FolderInstall:
        return "folder-install";
    case WizardButton::FolderRom:
        return "folder-rom";
    case WizardButton::FolderMedia:
        return "folder-media";
    case WizardButton::Lighting:
        return "lighting";
    case WizardButton::ReduceMotion:
        return "reduce-motion";
    }
    return "unknown";
}

std::optional<WizardButton> wizardButtonFromName(std::string_view name) noexcept {
    for (const WizardButton button : kButtons) {
        if (toString(button) == name) {
            return button;
        }
    }
    return std::nullopt;
}

std::string_view elementIdOf(WizardButton button) noexcept {
    switch (button) {
    case WizardButton::Next:
        return "wz-next";
    case WizardButton::Previous:
        return "wz-previous";
    case WizardButton::Quit:
        return "wz-quit";
    case WizardButton::Retry:
        return "wz-retry";
    case WizardButton::Enter:
        return "wz-enter";
    case WizardButton::FolderHome:
        return "wz-folder-home";
    case WizardButton::FolderInstall:
        return "wz-folder-install";
    case WizardButton::FolderRom:
        return "wz-folder-rom";
    case WizardButton::FolderMedia:
        return "wz-folder-media";
    case WizardButton::Lighting:
        return "wz-lighting";
    case WizardButton::ReduceMotion:
        return "wz-reduce-motion";
    }
    return {};
}

std::string_view toString(WizardFolder folder) noexcept {
    switch (folder) {
    case WizardFolder::Home:
        return "home";
    case WizardFolder::Install:
        return "install";
    case WizardFolder::Rom:
        return "rom";
    case WizardFolder::Media:
        return "media";
    }
    return "unknown";
}

std::string_view toString(WizardWork work) noexcept {
    switch (work) {
    case WizardWork::Idle:
        return "idle";
    case WizardWork::Running:
        return "running";
    case WizardWork::Done:
        return "done";
    case WizardWork::Failed:
        return "failed";
    }
    return "unknown";
}

std::string_view toString(WizardCommandKind kind) noexcept {
    switch (kind) {
    case WizardCommandKind::Quit:
        return "quit";
    case WizardCommandKind::Detect:
        return "detect";
    case WizardCommandKind::Browse:
        return "browse";
    case WizardCommandKind::Read:
        return "read";
    case WizardCommandKind::Import:
        return "import";
    case WizardCommandKind::Lighting:
        return "lighting";
    case WizardCommandKind::ReduceMotion:
        return "reduce-motion";
    case WizardCommandKind::StartWorld:
        return "start-world";
    case WizardCommandKind::Enter:
        return "enter";
    }
    return "unknown";
}

void WizardModel::open(bool resumed) {
    m_open = true;
    m_resumed = resumed;
    show(resumed ? WizardPage::Preview : WizardPage::Welcome);
}

void WizardModel::close() noexcept {
    if (m_open) {
        m_open = false;
        ++m_revision;
    }
}

std::optional<WizardCommand> WizardModel::takeEntered() noexcept {
    return std::exchange(m_entered, std::nullopt);
}

void WizardModel::show(WizardPage page) {
    m_page = page;
    m_failure.clear();
    switch (page) {
    case WizardPage::Detection:
        m_entered = WizardCommand{WizardCommandKind::Detect};
        break;
    case WizardPage::Preview:
        m_entered = WizardCommand{WizardCommandKind::Read};
        break;
    case WizardPage::Import:
        m_entered = WizardCommand{WizardCommandKind::Import};
        break;
    case WizardPage::World:
        m_entered = WizardCommand{WizardCommandKind::StartWorld};
        break;
    case WizardPage::Welcome:
    case WizardPage::Emulators:
    case WizardPage::Appearance:
        break;
    }
    const std::vector<WizardButton> shown = buttons();
    m_focused = shown.empty() ? WizardButton::Next : shown.front();
    if (canGoOn() && std::find(shown.begin(), shown.end(), WizardButton::Next) != shown.end()) {
        m_focused = WizardButton::Next;
    }
    ++m_revision;
}

std::vector<WizardButton> WizardModel::buttons() const {
    std::vector<WizardButton> shown;
    switch (m_page) {
    case WizardPage::Welcome:
        shown = {WizardButton::Next, WizardButton::Quit};
        break;
    case WizardPage::Detection:
        shown = {WizardButton::FolderHome, WizardButton::FolderInstall, WizardButton::FolderRom,
                 WizardButton::FolderMedia};
        if (canGoOn()) {
            shown.push_back(WizardButton::Next);
        } else if (m_detectionWork != WizardWork::Running) {
            shown.push_back(WizardButton::Retry);
        }
        shown.push_back(WizardButton::Previous);
        break;
    case WizardPage::Preview:
        if (m_previewWork == WizardWork::Done) {
            shown.push_back(WizardButton::Next);
        } else if (m_previewWork == WizardWork::Failed) {
            shown.push_back(WizardButton::Retry);
        }
        shown.push_back(WizardButton::Previous);
        break;
    case WizardPage::Import:
        if (m_importWork == WizardWork::Done) {
            shown.push_back(WizardButton::Next);
        } else if (m_importWork == WizardWork::Failed) {
            shown = {WizardButton::Retry, WizardButton::Previous};
        }
        break;
    case WizardPage::Emulators:
        shown = {WizardButton::Next};
        break;
    case WizardPage::Appearance:
        shown = {WizardButton::Lighting, WizardButton::ReduceMotion, WizardButton::Next, WizardButton::Previous};
        break;
    case WizardPage::World:
        if (m_worldWork == WizardWork::Done) {
            shown.push_back(WizardButton::Enter);
        }
        break;
    }
    return shown;
}

bool WizardModel::canGoOn() const noexcept {
    switch (m_page) {
    case WizardPage::Detection:
        return m_detectionWork == WizardWork::Done && m_detection.problem == "none";
    case WizardPage::Preview:
        return m_previewWork == WizardWork::Done;
    case WizardPage::Import:
        return m_importWork == WizardWork::Done;
    case WizardPage::World:
        return m_worldWork == WizardWork::Done;
    case WizardPage::Welcome:
    case WizardPage::Emulators:
    case WizardPage::Appearance:
        return true;
    }
    return false;
}

std::string WizardModel::focusedElement() const {
    return std::string(elementIdOf(m_focused));
}

void WizardModel::focus(WizardButton button) noexcept {
    if (m_focused != button) {
        m_focused = button;
        ++m_revision;
    }
}

void WizardModel::settleFocus() {
    const std::vector<WizardButton> shown = buttons();
    if (std::find(shown.begin(), shown.end(), m_focused) == shown.end() && !shown.empty()) {
        m_focused = shown.front();
    }
    // What a page waited for arrived: its way forward is focused, so that A goes on.
    if (canGoOn() && std::find(shown.begin(), shown.end(), WizardButton::Next) != shown.end() &&
        (m_focused == WizardButton::Previous || m_page == WizardPage::Import)) {
        m_focused = WizardButton::Next;
    }
    if (m_page == WizardPage::World && canGoOn()) {
        m_focused = WizardButton::Enter;
    }
    if ((m_page == WizardPage::Preview || m_page == WizardPage::Import) && !canGoOn() && !shown.empty() &&
        std::find(shown.begin(), shown.end(), WizardButton::Retry) != shown.end()) {
        m_focused = WizardButton::Retry;
    }
}

void WizardModel::setDetection(WizardWork work, WizardDetection detection) {
    const bool waited = m_detectionWork != WizardWork::Done;
    m_detectionWork = work;
    m_detection = std::move(detection);
    if (m_page == WizardPage::Detection) {
        settleFocus();
        if (waited && canGoOn()) {
            m_focused = WizardButton::Next;
        }
    }
    ++m_revision;
}

void WizardModel::setPreview(WizardWork work, WizardPreview preview, std::string failure) {
    m_previewWork = work;
    m_preview = preview;
    m_failure = std::move(failure);
    if (m_page == WizardPage::Preview) {
        settleFocus();
    }
    ++m_revision;
}

void WizardModel::setImport(WizardWork work, WizardImport progress, std::string failure) {
    m_importWork = work;
    m_import = std::move(progress);
    m_failure = std::move(failure);
    if (m_page == WizardPage::Import) {
        settleFocus();
    }
    ++m_revision;
}

void WizardModel::setAppearance(std::string lightingProfile, bool reduceMotion) {
    m_lighting = std::move(lightingProfile);
    m_reduceMotion = reduceMotion;
    ++m_revision;
}

void WizardModel::setWorld(WizardWork work) {
    m_worldWork = work;
    if (m_page == WizardPage::World) {
        settleFocus();
    }
    ++m_revision;
}

std::optional<WizardCommand> WizardModel::press(WizardButton button, int direction) {
    const auto browse = [](WizardFolder folder) {
        WizardCommand command{WizardCommandKind::Browse};
        command.folder = folder;
        return command;
    };
    switch (button) {
    case WizardButton::Next:
        if (!canGoOn() || m_page == WizardPage::World) {
            return std::nullopt;
        }
        show(static_cast<WizardPage>(static_cast<int>(m_page) + 1));
        return takeEntered();
    case WizardButton::Previous:
        if (m_page == WizardPage::Detection) {
            show(WizardPage::Welcome);
        } else if (m_page == WizardPage::Preview) {
            show(WizardPage::Detection);
            return takeEntered();
        } else if (m_page == WizardPage::Import && m_importWork == WizardWork::Failed) {
            show(WizardPage::Preview);
            return takeEntered();
        } else if (m_page == WizardPage::Appearance) {
            show(WizardPage::Emulators);
        }
        return std::nullopt;
    case WizardButton::Quit:
        return WizardCommand{WizardCommandKind::Quit};
    case WizardButton::Retry:
        if (m_page == WizardPage::Detection) {
            return WizardCommand{WizardCommandKind::Detect};
        }
        if (m_page == WizardPage::Preview) {
            return WizardCommand{WizardCommandKind::Read};
        }
        if (m_page == WizardPage::Import) {
            return WizardCommand{WizardCommandKind::Import};
        }
        return std::nullopt;
    case WizardButton::Enter:
        return canGoOn() ? std::optional<WizardCommand>(WizardCommand{WizardCommandKind::Enter}) : std::nullopt;
    case WizardButton::FolderHome:
        return browse(WizardFolder::Home);
    case WizardButton::FolderInstall:
        return browse(WizardFolder::Install);
    case WizardButton::FolderRom:
        return browse(WizardFolder::Rom);
    case WizardButton::FolderMedia:
        return browse(WizardFolder::Media);
    case WizardButton::Lighting: {
        WizardCommand command{WizardCommandKind::Lighting};
        command.direction = direction < 0 ? -1 : 1;
        return command;
    }
    case WizardButton::ReduceMotion:
        return WizardCommand{WizardCommandKind::ReduceMotion};
    }
    return std::nullopt;
}

std::optional<WizardCommand> WizardModel::apply(UiCommand command) {
    if (!m_open) {
        return std::nullopt;
    }
    const std::vector<WizardButton> shown = buttons();
    switch (command) {
    case UiCommand::Up:
    case UiCommand::Down: {
        if (shown.empty()) {
            return std::nullopt;
        }
        const auto at = std::find(shown.begin(), shown.end(), m_focused);
        if (at == shown.end()) {
            focus(shown.front());
        } else if (command == UiCommand::Up && at != shown.begin()) {
            focus(*(at - 1));
        } else if (command == UiCommand::Down && at + 1 != shown.end()) {
            focus(*(at + 1));
        }
        return std::nullopt;
    }
    case UiCommand::Left:
    case UiCommand::Right:
        if (m_page == WizardPage::Appearance && m_focused == WizardButton::Lighting) {
            return press(WizardButton::Lighting, command == UiCommand::Left ? -1 : 1);
        }
        if (m_page == WizardPage::Appearance && m_focused == WizardButton::ReduceMotion) {
            return press(WizardButton::ReduceMotion, 1);
        }
        return std::nullopt;
    case UiCommand::Activate: {
        // A second A on the way in presses nothing a page that is still looking offers: only clicks, which are aimed.
        const bool looking = (m_page == WizardPage::Detection && m_detectionWork != WizardWork::Done) ||
                             (m_page == WizardPage::Preview &&
                              (m_previewWork == WizardWork::Idle || m_previewWork == WizardWork::Running));
        if (looking) {
            return std::nullopt;
        }
        return std::find(shown.begin(), shown.end(), m_focused) != shown.end() ? press(m_focused, 1) : std::nullopt;
    }
    case UiCommand::Back:
        if (m_page == WizardPage::Welcome) {
            focus(WizardButton::Quit); // the way out, one more press away: nothing ends by mistake
            return std::nullopt;
        }
        if (std::find(shown.begin(), shown.end(), WizardButton::Previous) != shown.end()) {
            return press(WizardButton::Previous, 1);
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

std::optional<WizardCommand> WizardModel::click(WizardButton button, int direction) {
    if (!m_open) {
        return std::nullopt;
    }
    const std::vector<WizardButton> shown = buttons();
    if (std::find(shown.begin(), shown.end(), button) == shown.end()) {
        return std::nullopt;
    }
    focus(button);
    return press(button, direction);
}

} // namespace rv::ui
