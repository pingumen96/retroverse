#pragma once

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>

namespace rv::core {

/// Result of ArgParser::parse().
class ParsedArgs {
public:
    /// True when a flag or option was given on the command line.
    [[nodiscard]] bool has(std::string_view name) const;
    /// Option value (or its default when declared with one); the last one when the option was given several times.
    [[nodiscard]] std::optional<std::string> value(std::string_view name) const;
    /// Every value of an option given several times ("--system a --system b"), in the order given; its default alone
    /// when it was not given and has one; empty otherwise.
    [[nodiscard]] std::vector<std::string> values(std::string_view name) const;
    /// Integer option value, `fallback` when absent; ParseError when present but not an integer.
    [[nodiscard]] Expected<int> intValue(std::string_view name, int fallback) const;
    /// Floating point option value, `fallback` when absent; ParseError when malformed.
    [[nodiscard]] Expected<double> doubleValue(std::string_view name, double fallback) const;
    /// Value of a declared positional argument, by name.
    [[nodiscard]] std::optional<std::string> positional(std::string_view name) const;

private:
    friend class ArgParser;
    std::map<std::string, std::string, std::less<>> m_values; // options with values (explicit or default)
    std::map<std::string, bool, std::less<>> m_present;       // flags and options given explicitly
    std::map<std::string, std::string, std::less<>> m_positionals;
    std::map<std::string, std::vector<std::string>, std::less<>> m_given; // every value given, by option
};

/// Minimal GNU-style command line parser: `--flag`, `--option value`, `--option=value`, positionals.
/// `--help` / `-h` is always accepted and reported as the flag "help".
class ArgParser {
public:
    explicit ArgParser(std::string programName, std::string description = {});

    ArgParser& flag(std::string name, std::string help);
    ArgParser& option(std::string name, std::string valueName, std::string help,
                      std::optional<std::string> defaultValue = std::nullopt);
    ArgParser& positional(std::string name, std::string help, bool required = false);

    /// Parses arguments without the program name. Unknown options and surplus positionals are errors.
    [[nodiscard]] Expected<ParsedArgs> parse(std::span<const std::string> arguments) const;
    /// Convenience overload for main(): skips argv[0]. Arguments are expected to be UTF-8.
    [[nodiscard]] Expected<ParsedArgs> parse(int argc, const char* const* argv) const;

    [[nodiscard]] std::string helpText() const;

private:
    struct Spec {
        std::string name;
        std::string valueName; // empty for flags
        std::string help;
        std::optional<std::string> defaultValue;
    };
    struct PositionalSpec {
        std::string name;
        std::string help;
        bool required = false;
    };

    [[nodiscard]] const Spec* find(std::string_view name) const;

    std::string m_programName;
    std::string m_description;
    std::vector<Spec> m_specs;
    std::vector<PositionalSpec> m_positionals;
};

} // namespace rv::core
