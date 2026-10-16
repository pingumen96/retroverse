#pragma once

// Retroverse's own argument templates (ADR-0008 §8, specification §11.2): what an emulator profile passes to its
// program, written as a list of elements. Each element expands to exactly one argument and is never split again, so
// that a ROM path stays one argument whatever it holds (spaces, quotes, '&', '%', '^', ';', '=', non-ASCII text, any
// length); buildCommandLine() then quotes it for CreateProcessW. ES-DE's own commands are translated by the catalog
// (I2 step 10), not here.
//
// Tokens stand between braces and may be glued to text ("-image={rom}"); "{{" and "}}" are literal braces:
//   {rom}           the ROM: its path as the launch names it (absolute), '\' between the elements; a folder for the
//                   games that are folders
//   {rom_dir}       the folder holding the ROM
//   {rom_name}      the ROM's name without its last extension ("Mole Mania (USA)")
//   {rom_file}      the ROM's name ("Mole Mania (USA).gb")
//   {rom_ext}       the ROM's last extension with its dot (".gb"), empty when it has none: "{rom_name}{rom_ext}" is
//                   always "{rom_file}"
//   {emulator_dir}  the folder of the profile's executable
//   {system}        the ES-DE system name ("gb")
//   {var:NAME}      a variable: the launch's, else the system's, else the profile's (TemplateContext::variables); its
//                   value is a template itself, expanded where it is used
//   {env:NAME}      an environment variable of the allow-list (folders of the user and of the system), never another
// An unknown token, an undefined variable, an environment variable outside the allow-list or not set are errors naming
// it; nothing is ever left unexpanded or passed on as written. The work an element takes is bounded whatever its
// variables hold (kMaxTemplateTokens, kMaxExpandedBytes): a variable whose value uses another one twice, and so on,
// would otherwise double the work with every level.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/launcher/command_line.hpp>

namespace rv::launcher {

/// The tokens one element may expand, those in the values of its variables included (each {var:NAME} counts, then the
/// tokens of its value): far more than any configuration needs, few enough that an element always expands at once.
inline constexpr std::size_t kMaxTemplateTokens = 1024;

/// The longest expansion of one element, in UTF-8 bytes: UTF-8 takes at most 3 bytes per UTF-16 code unit, so longer
/// text cannot fit a command line (kMaxCommandLineUnits), nor a path or an environment value, which Windows limits
/// alike.
inline constexpr std::size_t kMaxExpandedBytes = 3 * kMaxCommandLineUnits;

/// What a token stands for.
enum class TemplateToken : std::uint8_t {
    Rom,         ///< {rom}
    RomDir,      ///< {rom_dir}
    RomName,     ///< {rom_name}
    RomFile,     ///< {rom_file}
    RomExt,      ///< {rom_ext}
    EmulatorDir, ///< {emulator_dir}
    System,      ///< {system}
    Variable,    ///< {var:NAME}
    Environment, ///< {env:NAME}
};

/// The token as written without its braces and name: "rom", "rom_dir", ..., "var", "env".
[[nodiscard]] std::string_view toString(TemplateToken token) noexcept;

/// One piece of a parsed element: literal text (no token), or a token with, for Variable and Environment, its NAME.
struct TemplatePart {
    std::optional<TemplateToken> token;
    /// The literal text (braces unescaped), or the NAME of {var:NAME} and {env:NAME}; empty for the other tokens.
    std::string text;

    friend bool operator==(const TemplatePart&, const TemplatePart&) = default;
};

/// Splits one element into literal text and tokens; adjacent literal text is one part. Errors (ParseError, the message
/// naming the token or the brace, the element as context): a '{' without its '}', a '}' alone (literal braces are
/// written "{{" and "}}"), an empty or unknown token ("{ROM}", "{romname}": tokens are lower case), {var:NAME} with a
/// NAME that isValidVariableName() refuses, {env:NAME} with an empty NAME or one holding '=' (whether the allow-list
/// holds it is checked where it is expanded or by checkTemplateElement()).
[[nodiscard]] Expected<std::vector<TemplatePart>> parseTemplateElement(std::string_view element);

/// A variable's name: 1 to 64 ASCII letters, digits, '_', '-' or '.'; compared exactly (case matters).
[[nodiscard]] bool isValidVariableName(std::string_view name) noexcept;

/// The environment variables {env:NAME} may read, upper case: folders of the user and of the system (APPDATA,
/// LOCALAPPDATA, PROGRAMFILES, SYSTEMDRIVE, USERPROFILE, ...). Any other one is refused, so that a template cannot copy
/// what the environment holds besides (tokens, passwords) into a command line, which is logged.
[[nodiscard]] std::span<const std::string_view> templateEnvironmentAllowList() noexcept;
/// True when `name` is in templateEnvironmentAllowList(), compared without regard to ASCII case as Windows compares
/// variable names.
[[nodiscard]] bool isAllowedEnvironmentVariable(std::string_view name) noexcept;

/// Reads an environment variable: its value, nothing when it is not set (or empty: no folder is named by an empty
/// value).
using EnvironmentLookup = std::function<std::optional<std::string>(std::string_view name)>;
/// The process's own environment (processEnvironmentVariable(), an empty value read as not set).
[[nodiscard]] EnvironmentLookup processEnvironmentLookup();

/// Variables by name; each value is a template. std::less<> so that names are looked up as string_views.
using TemplateVariables = std::map<std::string, std::string, std::less<>>;

/// The values tokens expand to. What is missing makes its tokens errors (InvalidArgument: "{rom} has no value here").
struct TemplateContext {
    /// The ROM, for {rom}, {rom_dir}, {rom_name}, {rom_file} and {rom_ext}.
    std::optional<std::filesystem::path> rom;
    /// {system}.
    std::optional<std::string> system;
    /// {emulator_dir}.
    std::optional<std::filesystem::path> emulatorDirectory;
    /// The scopes of {var:NAME}, most specific first: the first scope holding NAME gives its value. A variable's value
    /// is expanded in this same context, its own {var:...} looked up from the first scope again.
    std::vector<const TemplateVariables*> variables;
    /// {env:NAME}, asked with the allow-list's spelling of NAME ("{env:ProgramFiles}" asks for "PROGRAMFILES");
    /// nullptr: such tokens are errors.
    EnvironmentLookup environment;
};

/// Expands one element into one argument (an element that expands to nothing is an empty argument). Path tokens give
/// their path in UTF-8 with '\' between the elements (a trailing separator dropped). Errors, the token or variable
/// named in the message and the element as context:
///  - ParseError: what parseTemplateElement() refuses, also in a variable's value (named);
///  - NotFound: {var:NAME} defined in no scope; {env:NAME} not set;
///  - ValidationFailed: {env:NAME} outside the allow-list; variables that refer to each other (the cycle named) or
///    nest deeper than 32; more than kMaxTemplateTokens tokens expanded (the token and the variable being expanded
///    named) or a result longer than kMaxExpandedBytes, both found as soon as the limit is passed; a result holding a
///    NUL character or malformed UTF-8 (a variable given by the launch); a path that UTF-8 cannot represent;
///  - InvalidArgument: a token whose value the context does not have.
[[nodiscard]] Expected<std::string> expandTemplateElement(std::string_view element, const TemplateContext& context);

/// expandTemplateElement() for each element, in order: one argument per element. An error names the element by its
/// index in its context ("arguments[2]: ...").
[[nodiscard]] Expected<std::vector<std::string>> expandTemplate(std::span<const std::string> elements,
                                                                const TemplateContext& context);

/// Checks `element` as expandTemplateElement() would expand it, with the same errors and limits, but without the values
/// of the path tokens ({rom}, {rom_dir}, {rom_name}, {rom_file}, {rom_ext}, {emulator_dir}, {system}), which are
/// accepted whether the context has them or not (and count as tokens): whether a profile's templates can be expanded
/// for a system before its executable is located (launch_plan.hpp).
[[nodiscard]] Status checkTemplateElement(std::string_view element, const TemplateContext& context);

} // namespace rv::launcher
