// I11 step 13 (ADR-0020 §3): ES-DE's "open the game's file" translated natively, never through cmd.exe.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include <rv/esde/launch_translation.hpp>

#include "launch_test_machine.hpp"
#include "support/program_stub.hpp"

namespace fs = std::filesystem;
using rv::catalog::LaunchReadiness;
using rv::esde::FindRule;
using rv::esde::FindRuleType;
using rv::launcher::WaitStrategy;

namespace {

constexpr const char* kOpenFile = "%HIDEWINDOW% %ESCAPESPECIALS% %EMULATOR_OS-SHELL% /C %ROM%";

std::string joined(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) {
        text += line + '\n';
    }
    return text;
}

/// A machine whose OS-SHELL is cmd.exe, as ES-DE's find rules find it, and the "windows" system's games.
struct ShellMachine {
    rv::test::LaunchMachine machine;
    fs::path shell;

    ShellMachine() {
        machine.emulator("OS-SHELL", {FindRule{FindRuleType::SystemPath, {"cmd.exe"}}});
        shell = machine.program(machine.root() / "Windows" / "System32" / "cmd.exe");
        machine.protocols.push_back(
            rv::launcher::ProtocolRule{"steam", {"steam.exe", "steamwebhelper.exe"}, {}, "emulators.json"});
    }

    [[nodiscard]] fs::path game(const std::string& name) const { return machine.roms() / "windows" / name; }

    rv::catalog::LaunchTranslation translate(const std::string& name, const std::string& command = kOpenFile) {
        const rv::catalog::LaunchGame game = rv::test::testGame(name);
        const rv::esde::SystemDefinition system = rv::test::testSystem({{"Shortcut or script", command}}, "windows");
        return rv::esde::translateCommand(machine.context(), system, system.commands.front(), &game);
    }

    /// A shortcut `name` the probe reads as opening `target` with `arguments`, in `folder` (empty: none).
    void shortcut(const std::string& name, const fs::path& target, std::string arguments = {}, fs::path folder = {},
                  bool administrator = false) {
        machine.file(game(name), "shortcut");
        machine.probe.addShortcut(game(name), rv::core::ShellShortcut{target, std::move(arguments), std::move(folder),
                                                                     administrator});
    }
};

} // namespace

TEST_CASE("esde/open the game's file: a shortcut starts its program, never cmd.exe", "[esde][launch][shortcut]") {
    ShellMachine m;
    const fs::path program = m.machine.program(m.machine.root() / "Games" / "Kettle" / "kettle.exe");
    const fs::path data = m.machine.root() / "Games" / "Kettle" / "data";
    fs::create_directories(data);

    SECTION("its target, its arguments split as CommandLineToArgvW splits them, its working folder") {
        m.shortcut("Kettle.lnk", program, "-w \"two words\" c\\\"d --last", data);
        const auto translation = m.translate("Kettle.lnk");
        INFO(rv::test::blockerText(translation));
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        REQUIRE(translation.request);
        CHECK(translation.request->executable == program);
        CHECK(translation.request->arguments == std::vector<std::string>{"-w", "two words", "c\"d", "--last"});
        CHECK(translation.request->workingDirectory == data);
        CHECK(translation.request->waitStrategy == WaitStrategy::DirectProcess);
        CHECK_FALSE(translation.request->hideConsoleWindow);
        CHECK(translation.request->label == "Kettle.lnk with Shortcut or script");
        REQUIRE(translation.program);
        CHECK(translation.program->path == program);
        CHECK(translation.program->rule == "shortcut");
        CHECK(translation.emulator == "OS-SHELL");
    }
    SECTION("no working folder: %STARTDIR%'s, else the target's") {
        m.shortcut("Kettle.lnk", program);
        auto translation = m.translate("Kettle.lnk");
        REQUIRE(translation.request);
        CHECK(translation.request->workingDirectory == program.parent_path());
        CHECK(translation.request->arguments.empty());
        translation =
            m.translate("Kettle.lnk", "%HIDEWINDOW% %ESCAPESPECIALS% %STARTDIR%=%GAMEDIR% %EMULATOR_OS-SHELL% /C %ROM%");
        REQUIRE(translation.request);
        CHECK(translation.request->workingDirectory == m.game("Kettle.lnk").parent_path());
    }
    SECTION("%RUNINBACKGROUND%: the game's window followed") {
        m.shortcut("Kettle.lnk", program);
        const auto translation =
            m.translate("Kettle.lnk", "%HIDEWINDOW% %ESCAPESPECIALS% %RUNINBACKGROUND% %EMULATOR_OS-SHELL% /C %ROM%");
        REQUIRE(translation.request);
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        CHECK(translation.request->waitStrategy == WaitStrategy::FollowWindow);
    }
    SECTION("administrator rights are not requested") {
        m.shortcut("Kettle.lnk", program, {}, {}, true);
        const auto translation = m.translate("Kettle.lnk");
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        CHECK(joined(translation.notes).find("administrator") != std::string::npos);
    }
    SECTION("a shortcut to what Retroverse never starts") {
        const fs::path batch = m.machine.file(m.machine.root() / "Games" / "start.bat", "@echo off");
        m.machine.probe.addFile(batch);
        m.shortcut("Batch.lnk", batch);
        CHECK(rv::test::blockerCodes(m.translate("Batch.lnk")) == std::vector<std::string>{"batch-file"});
        const fs::path document = m.machine.file(m.machine.root() / "Games" / "manual.pdf", "%PDF");
        m.shortcut("Manual.lnk", document);
        CHECK(rv::test::blockerCodes(m.translate("Manual.lnk")) == std::vector<std::string>{"shortcut-target"});
        m.shortcut("Shell.lnk", m.shell, "/C del x");
        CHECK(rv::test::blockerCodes(m.translate("Shell.lnk")) == std::vector<std::string>{"program-refused"});
        m.shortcut("Gone.lnk", m.machine.root() / "Games" / "gone.exe");
        const auto gone = m.translate("Gone.lnk");
        CHECK(gone.readiness() == LaunchReadiness::EmulatorNotFound);
        CHECK(rv::test::blockerCodes(gone) == std::vector<std::string>{"emulator-not-found"});
        m.machine.file(m.game("Unread.lnk"), "not a shortcut");
        CHECK(rv::test::blockerCodes(m.translate("Unread.lnk")) == std::vector<std::string>{"shortcut-unreadable"});
    }
}

TEST_CASE("esde/open the game's file: a program, an Internet Shortcut, and what is neither", "[esde][launch][shortcut]") {
    ShellMachine m;

    SECTION("a program starts as itself, its console hidden by %HIDEWINDOW%") {
        const fs::path game = m.machine.program(m.game("Tool Kit") / "kit.exe");
        const auto translation = m.translate("Tool Kit/kit.exe");
        INFO(rv::test::blockerText(translation));
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        REQUIRE(translation.request);
        CHECK(translation.request->executable == game);
        CHECK(translation.request->arguments.empty());
        CHECK(translation.request->workingDirectory == game.parent_path());
        CHECK(translation.request->hideConsoleWindow);
        REQUIRE(translation.program);
        CHECK(translation.program->rule == "game-file");
    }
    SECTION("an Internet Shortcut of a listed protocol is activated, its window followed") {
        m.machine.file(m.game("Lantern.url"), "\xEF\xBB\xBF[{000214A0-0000-0000-C000-000000000046}]\r\nProp3=19,0\r\n"
                                              "[InternetShortcut]\r\nIDList=\r\nurl = steam://rungameid/400 \r\n");
        const auto translation = m.translate("Lantern.url");
        INFO(rv::test::blockerText(translation));
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        REQUIRE(translation.request);
        CHECK(translation.request->isActivation());
        CHECK(translation.request->activationUrl == "steam://rungameid/400");
        CHECK(translation.request->executable.empty());
        CHECK(translation.request->waitStrategy == WaitStrategy::FollowWindow);
        CHECK(translation.request->launcherProcesses == std::vector<std::string>{"steam.exe", "steamwebhelper.exe"});
        REQUIRE(translation.program);
        CHECK(translation.program->rule == "internet-shortcut");
    }
    SECTION("an Internet Shortcut of any other scheme is never activated") {
        m.machine.file(m.game("Web.url"), "[InternetShortcut]\nURL=https://example.com/game\n");
        CHECK(rv::test::blockerCodes(m.translate("Web.url")) == std::vector<std::string>{"protocol-not-listed"});
        m.machine.file(m.game("Store.url"), "[InternetShortcut]\nURL=uplay://launch/1\n");
        CHECK(rv::test::blockerCodes(m.translate("Store.url")) == std::vector<std::string>{"protocol-not-listed"});
        m.machine.protocols.clear();
        m.machine.file(m.game("Lantern.url"), "[InternetShortcut]\nURL=steam://rungameid/400\n");
        CHECK(rv::test::blockerCodes(m.translate("Lantern.url")) == std::vector<std::string>{"protocol-not-listed"});
    }
    SECTION("an Internet Shortcut without a URL, or with one never activated") {
        m.machine.file(m.game("Empty.url"), "[Other]\nURL=steam://rungameid/1\n");
        CHECK(rv::test::blockerCodes(m.translate("Empty.url")) == std::vector<std::string>{"shortcut-unreadable"});
        m.machine.file(m.game("Spaced.url"), "[InternetShortcut]\nURL=steam://run game/1\n");
        CHECK(rv::test::blockerCodes(m.translate("Spaced.url")) == std::vector<std::string>{"invalid-request"});
    }
    SECTION("a batch file, a disc image") {
        m.machine.file(m.game("start.bat"), "@echo off");
        CHECK(rv::test::blockerCodes(m.translate("start.bat")) == std::vector<std::string>{"batch-file"});
        m.machine.file(m.game("Disc (Europe).iso"), "iso");
        const auto image = m.translate("Disc (Europe).iso");
        CHECK(image.readiness() == LaunchReadiness::RequiresConfiguration);
        CHECK(rv::test::blockerCodes(image) == std::vector<std::string>{"file-not-opened"});
        CHECK(rv::test::blockerText(image).find("choose another command") != std::string::npos);
    }
    SECTION("no shell found: what ES-DE refuses") {
        rv::test::LaunchMachine bare;
        bare.file(bare.roms() / "windows" / "kit.exe");
        const rv::catalog::LaunchGame game = rv::test::testGame("kit.exe");
        const rv::esde::SystemDefinition system = rv::test::testSystem({{"Shortcut or script", kOpenFile}}, "windows");
        const auto translation = rv::esde::translateCommand(bare.context(), system, system.commands.front(), &game);
        CHECK(rv::test::blockerCodes(translation) ==
              std::vector<std::string>{"find-rules-missing", "token-unsupported"});
        CHECK_FALSE(translation.request);
    }
    SECTION("a system: each game's file, no program") {
        const rv::esde::SystemDefinition system = rv::test::testSystem({{"Shortcut or script", kOpenFile}}, "windows");
        const auto translation =
            rv::esde::translateCommand(m.machine.context(), system, system.commands.front(), nullptr);
        CHECK(translation.readiness() == LaunchReadiness::Ready);
        CHECK_FALSE(translation.program);
        CHECK_FALSE(translation.request);
        REQUIRE(translation.notes.size() == 1);
        CHECK(translation.notes.front().find("never through cmd.exe") != std::string::npos);
    }
}
