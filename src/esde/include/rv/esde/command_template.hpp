#pragma once

// ES-DE launch commands (the <command> elements of es_systems.xml) as argv elements with their tokens, never expanded
// (ADR-0008 §8): a command is split the way Windows' CommandLineToArgvW splits a command line, before any token is
// expanded, so that a value expanded later (a ROM path holding spaces, quotes or '%') is one argument and is never
// split or expanded again. The launch translation (I2 step 10) decides the program, expands the tokens and refuses
// what it cannot run; nothing here looks at the file system.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rv::esde {

/// Splits `commandLine` (UTF-8) into its elements exactly as shell32's CommandLineToArgvW splits a command line:
///  - the first element follows the program-name rule: when it starts with '"' it runs to the next '"' (backslashes are
///    literal, the quotes are dropped, and whatever follows the closing quote starts the next element); otherwise it
///    runs to the first character from U+0001 to U+0020 (one such character is consumed), quotes and backslashes
///    literal;
///  - the other elements are separated by spaces and tabs outside quotes; 2n backslashes before a '"' give n
///    backslashes and the quote opens or closes a quoted part, 2n+1 give n backslashes and a literal '"'; backslashes
///    before anything else are literal; inside a quoted part three consecutive quotes give one literal '"' (the
///    count goes on modulo 3: one closes the part, two close it and give a '"'); "" is an empty element.
/// The text ends at a NUL character, as the C string CommandLineToArgvW reads does. Nothing for an empty text (where
/// CommandLineToArgvW returns the calling program's own path); a text of blanks gives one empty element. Every special
/// character is ASCII, so splitting UTF-8 bytes gives the elements CommandLineToArgvW gives in UTF-16.
[[nodiscard]] std::vector<std::string> splitCommandLine(std::string_view commandLine);

/// The kinds of tokens in an ES-DE command (FileData::launchGame of ES-DE 3.4.1 on Windows).
enum class CommandTokenKind : std::uint8_t {
    /// A variable ES-DE replaces with a value: %ROM%, %ROMRAW%, %ROMPATH%, %BASENAME%, %FILENAME%, %GAMEDIR%,
    /// %GAMEDIRRAW%, %EMUDIR%, %ESPATH%, %EMUPATH% (the emulator's folder, in core paths) and %GAMEENTRYDIR% (which
    /// ES-DE replaces only inside the value of %STARTDIR%=).
    Variable,
    /// A flag ES-DE removes from the command, launching differently: %HIDEWINDOW%, %ESCAPESPECIALS%,
    /// %RUNINBACKGROUND%.
    Flag,
    /// %STARTDIR%= or %INJECT%=: the rest of the element is its value (the working directory, the file whose content
    /// ES-DE injects as arguments). CommandToken::text ends with the '='.
    Assignment,
    /// %EMULATOR_<name>%: the program, found through the find rules of `name` (CommandToken::target).
    Emulator,
    /// %CORE_<name>%: a folder found through the core rules of `name` (CommandToken::target).
    Core,
    /// %PRECOMMAND_<name>%: a program ES-DE puts before the command, found through the find rules of `name`.
    PreCommand,
    /// '~': ES-DE replaces every '~' of a command with its home folder (the parent of the ES-DE home) before anything
    /// else.
    HomeFolder,
    /// %STARTDIR% or %INJECT% without its '=': ES-DE refuses to launch ("invalid ... entry").
    Malformed,
    /// Any other %NAME% (a letter or '_', then letters, digits and "_-!.()"): text ES-DE leaves as it is on Windows,
    /// such as %RPCS3_GAMEID%, the tokens of its other platforms (%ROMRAWWIN%, %ENABLESHORTCUTS%, %ANDROIDAPP%=, ...)
    /// or an environment variable (%APPDATA%), which nothing expands either.
    Unknown,
};

/// "variable", "flag", "assignment", "emulator", "core", "pre-command", "home-folder", "malformed", "unknown".
[[nodiscard]] std::string_view toString(CommandTokenKind kind) noexcept;

/// One token of an element.
struct CommandToken {
    CommandTokenKind kind = CommandTokenKind::Unknown;
    /// Where it starts in the element's text (bytes).
    std::size_t offset = 0;
    /// As written: "%ROM%", "%STARTDIR%=", "%EMULATOR_RETROARCH%", "~".
    std::string text;
    /// The name between the '%' characters ("ROM", "STARTDIR", "EMULATOR_RETROARCH"); "~" for HomeFolder.
    std::string name;
    /// For Emulator, Core and PreCommand: the find rules' name ("RETROARCH", "OS-SHELL", "PLAY!"); empty otherwise.
    std::string target;

    bool operator==(const CommandToken&) const = default;
};

/// One argv element of a command, as written once its quotes and escapes are resolved, and its tokens in order.
struct CommandElement {
    std::string text;
    std::vector<CommandToken> tokens;

    bool operator==(const CommandElement&) const = default;
};

/// A command split into elements.
struct CommandTemplate {
    std::vector<CommandElement> elements;
    /// The index of the element ES-DE starts as the program: the first one after the flags and the %STARTDIR%=
    /// elements that lead the command (ES-DE removes those before it launches); elements.size() when there is none.
    std::size_t programIndex = 0;

    bool operator==(const CommandTemplate&) const = default;
};

/// The tokens of one element, in order. Known tokens are matched exactly (case-sensitively, as ES-DE does), and
/// neither a stray '%' nor an unknown name before them hides one ("50%-%ROM%" and "x%y%ROM%" hold %ROM%); for
/// %EMULATOR_, %CORE_ and %PRECOMMAND_ the name runs to the next '%', as ES-DE reads it.
[[nodiscard]] std::vector<CommandToken> scanCommandTokens(std::string_view element);

/// Splits an ES-DE command into elements and lists their tokens. The elements are those of splitCommandLine(), with one
/// difference that follows ES-DE: what leads the command and ES-DE removes before it launches it is read as ES-DE's
/// launchGame() reads it, one element each, and the program-name rule applies to what follows, the program of the
/// command line ES-DE runs. A flag (%HIDEWINDOW%, %ESCAPESPECIALS%, %RUNINBACKGROUND%) is removed where the text starts
/// with it, whatever follows ("%HIDEWINDOW%%EMULATOR_X%"), then the blanks ES-DE trims (std::isspace: a line feed
/// too). %STARTDIR%= takes a value to the next space when it does not start with '"' (a tab is part of it), else the
/// text up to the next '"', escapes and all, and the one character after that quote is dropped; the element is
/// "%STARTDIR%=" and the value (%STARTDIR%="C:\Games dir" gives "%STARTDIR%=C:\Games dir"). A quoted value without its
/// closing quote, which ES-DE refuses, takes the rest of the command: no program follows (programIndex is
/// elements.size()). For a command that starts with its program, the elements are exactly splitCommandLine()'s.
[[nodiscard]] CommandTemplate parseCommandTemplate(std::string_view command);

/// The key a census of tokens counts a token under: "%EMULATOR_X%", "%CORE_X%" and "%PRECOMMAND_X%" for the named
/// kinds, the token's text otherwise ("%ROM%", "%STARTDIR%=", "~").
[[nodiscard]] std::string censusKey(const CommandToken& token);

} // namespace rv::esde
