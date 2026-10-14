#include <rv/core/arg_parser.hpp>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <sstream>

namespace rv::core {

// --- ParsedArgs -----------------------------------------------------------------------------------

bool ParsedArgs::has(std::string_view name) const {
    return m_present.find(name) != m_present.end();
}

std::optional<std::string> ParsedArgs::value(std::string_view name) const {
    const auto it = m_values.find(name);
    if (it == m_values.end()) {
        return std::nullopt;
    }
    return it->second;
}

Expected<int> ParsedArgs::intValue(std::string_view name, int fallback) const {
    const auto text = value(name);
    if (!text) {
        return fallback;
    }
    int parsed = 0;
    const char* begin = text->data();
    const char* end = text->data() + text->size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end) {
        return makeError(ErrorCode::ParseError, "expected an integer value", "--" + std::string(name) + "=" + *text);
    }
    return parsed;
}

Expected<double> ParsedArgs::doubleValue(std::string_view name, double fallback) const {
    const auto text = value(name);
    if (!text) {
        return fallback;
    }
    double parsed = 0.0;
    const char* begin = text->data();
    const char* end = text->data() + text->size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end) {
        return makeError(ErrorCode::ParseError, "expected a number", "--" + std::string(name) + "=" + *text);
    }
    return parsed;
}

std::optional<std::string> ParsedArgs::positional(std::string_view name) const {
    const auto it = m_positionals.find(name);
    if (it == m_positionals.end()) {
        return std::nullopt;
    }
    return it->second;
}

// --- ArgParser ------------------------------------------------------------------------------------

ArgParser::ArgParser(std::string programName, std::string description)
    : m_programName(std::move(programName)), m_description(std::move(description)) {}

ArgParser& ArgParser::flag(std::string name, std::string help) {
    m_specs.push_back(Spec{std::move(name), {}, std::move(help), std::nullopt});
    return *this;
}

ArgParser& ArgParser::option(std::string name, std::string valueName, std::string help,
                             std::optional<std::string> defaultValue) {
    if (valueName.empty()) {
        valueName = "value";
    }
    m_specs.push_back(Spec{std::move(name), std::move(valueName), std::move(help), std::move(defaultValue)});
    return *this;
}

ArgParser& ArgParser::positional(std::string name, std::string help, bool required) {
    m_positionals.push_back(PositionalSpec{std::move(name), std::move(help), required});
    return *this;
}

const ArgParser::Spec* ArgParser::find(std::string_view name) const {
    const auto it = std::find_if(m_specs.begin(), m_specs.end(), [&](const Spec& spec) { return spec.name == name; });
    return it == m_specs.end() ? nullptr : &*it;
}

Expected<ParsedArgs> ArgParser::parse(std::span<const std::string> arguments) const {
    ParsedArgs result;
    for (const Spec& spec : m_specs) {
        if (spec.defaultValue) {
            result.m_values[spec.name] = *spec.defaultValue;
        }
    }

    std::size_t positionalIndex = 0;
    bool onlyPositionals = false;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];

        if (!onlyPositionals && argument == "--") {
            onlyPositionals = true;
            continue;
        }
        if (!onlyPositionals && (argument == "--help" || argument == "-h")) {
            result.m_present["help"] = true;
            continue;
        }
        if (!onlyPositionals && argument.size() > 2 && argument.starts_with("--")) {
            std::string_view body(argument);
            body.remove_prefix(2);
            std::optional<std::string> inlineValue;
            if (const auto equals = body.find('='); equals != std::string_view::npos) {
                inlineValue = std::string(body.substr(equals + 1));
                body = body.substr(0, equals);
            }
            const Spec* spec = find(body);
            if (spec == nullptr) {
                return makeError(ErrorCode::InvalidArgument, "unknown option", argument);
            }
            if (spec->valueName.empty()) {
                if (inlineValue) {
                    return makeError(ErrorCode::InvalidArgument, "flag does not take a value", argument);
                }
                result.m_present[spec->name] = true;
                continue;
            }
            if (!inlineValue) {
                if (i + 1 >= arguments.size()) {
                    return makeError(ErrorCode::InvalidArgument, "missing value for option", argument);
                }
                inlineValue = arguments[++i];
            }
            result.m_values[spec->name] = *inlineValue;
            result.m_present[spec->name] = true;
            continue;
        }
        if (!onlyPositionals && argument.size() > 1 && argument.front() == '-' &&
            !(argument[1] >= '0' && argument[1] <= '9')) {
            return makeError(ErrorCode::InvalidArgument, "unknown option (use --name)", argument);
        }
        if (positionalIndex >= m_positionals.size()) {
            return makeError(ErrorCode::InvalidArgument, "unexpected argument", argument);
        }
        result.m_positionals[m_positionals[positionalIndex].name] = argument;
        ++positionalIndex;
    }

    if (!result.has("help")) {
        for (std::size_t p = positionalIndex; p < m_positionals.size(); ++p) {
            if (m_positionals[p].required) {
                return makeError(ErrorCode::InvalidArgument, "missing required argument", m_positionals[p].name);
            }
        }
    }
    return result;
}

Expected<ParsedArgs> ArgParser::parse(int argc, const char* const* argv) const {
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) {
        arguments.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
    return parse(arguments);
}

std::string ArgParser::helpText() const {
    std::ostringstream out;
    out << "Usage: " << m_programName << " [options]";
    for (const PositionalSpec& p : m_positionals) {
        out << (p.required ? " <" : " [") << p.name << (p.required ? ">" : "]");
    }
    out << "\n";
    if (!m_description.empty()) {
        out << "\n" << m_description << "\n";
    }
    if (!m_positionals.empty()) {
        out << "\nArguments:\n";
        for (const PositionalSpec& p : m_positionals) {
            out << "  " << p.name << "\n      " << p.help << "\n";
        }
    }
    out << "\nOptions:\n  -h, --help\n      Show this help and exit.\n";
    for (const Spec& spec : m_specs) {
        out << "  --" << spec.name;
        if (!spec.valueName.empty()) {
            out << " <" << spec.valueName << ">";
        }
        out << "\n      " << spec.help;
        if (spec.defaultValue) {
            out << " (default: " << *spec.defaultValue << ")";
        }
        out << "\n";
    }
    return out.str();
}

} // namespace rv::core
