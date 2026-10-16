#include <rv/launcher/arg_template.hpp>

#include <algorithm>
#include <array>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <rv/core/utf8.hpp>
#include <rv/launcher/environment.hpp>

namespace rv::launcher {
namespace {

constexpr std::size_t kMaxVariableNameLength = 64;
/// Variables whose values use variables: deeper than this is a mistake, not a configuration.
constexpr std::size_t kMaxNesting = 32;

constexpr std::string_view kVariablePrefix = "var:";
constexpr std::string_view kEnvironmentPrefix = "env:";

/// Folders of the user and of the system, the only environment variables a template reads.
constexpr std::array<std::string_view, 19> kEnvironmentAllowList{
    "ALLUSERSPROFILE",   "APPDATA",      "COMMONPROGRAMFILES", "COMMONPROGRAMFILES(X86)", "COMMONPROGRAMW6432",
    "HOMEDRIVE",         "HOMEPATH",     "LOCALAPPDATA",       "PROGRAMDATA",             "PROGRAMFILES",
    "PROGRAMFILES(X86)", "PROGRAMW6432", "PUBLIC",             "SYSTEMDRIVE",             "SYSTEMROOT",
    "TEMP",              "TMP",          "USERPROFILE",        "WINDIR",
};

struct NamedToken {
    std::string_view name;
    TemplateToken token;
};

/// The tokens without a name.
constexpr std::array<NamedToken, 7> kPlainTokens{{
    {"rom", TemplateToken::Rom},
    {"rom_dir", TemplateToken::RomDir},
    {"rom_name", TemplateToken::RomName},
    {"rom_file", TemplateToken::RomFile},
    {"rom_ext", TemplateToken::RomExt},
    {"emulator_dir", TemplateToken::EmulatorDir},
    {"system", TemplateToken::System},
}};

[[nodiscard]] tl::unexpected<Error> failure(ErrorCode code, std::string message, std::string_view element) {
    return makeError(code, std::move(message), std::string(element));
}

/// The token as written: "{rom}", "{var:core}".
[[nodiscard]] std::string written(const TemplatePart& part) {
    if (!part.token) {
        return part.text;
    }
    if (*part.token == TemplateToken::Variable || *part.token == TemplateToken::Environment) {
        return fmt::format("{{{}:{}}}", toString(*part.token), part.text);
    }
    return fmt::format("{{{}}}", toString(*part.token));
}

/// The token between braces, `body` without them.
[[nodiscard]] Expected<TemplatePart> parseToken(std::string_view body, std::string_view element) {
    if (body.empty()) {
        return failure(ErrorCode::ParseError, "empty token {} (a literal brace is written \"{{\" or \"}}\")", element);
    }
    for (const NamedToken& plain : kPlainTokens) {
        if (body == plain.name) {
            return TemplatePart{plain.token, {}};
        }
    }
    if (body.starts_with(kVariablePrefix)) {
        const std::string_view name = body.substr(kVariablePrefix.size());
        if (!isValidVariableName(name)) {
            return failure(ErrorCode::ParseError,
                           fmt::format("{{{}}} needs a variable name of 1 to {} ASCII letters, digits, '_', '-' or '.'",
                                       body, kMaxVariableNameLength),
                           element);
        }
        return TemplatePart{TemplateToken::Variable, std::string(name)};
    }
    if (body.starts_with(kEnvironmentPrefix)) {
        const std::string_view name = body.substr(kEnvironmentPrefix.size());
        if (name.empty() || name.find_first_of(std::string_view("=\0", 2)) != std::string_view::npos) {
            return failure(ErrorCode::ParseError, fmt::format("{{{}}} needs an environment variable name", body),
                           element);
        }
        return TemplatePart{TemplateToken::Environment, std::string(name)};
    }
    return failure(ErrorCode::ParseError, fmt::format("unknown token {{{}}}", body), element);
}

/// `path` with '\' between its elements and without a trailing separator ("C:\Games\Game.ps3\" names the folder
/// Game.ps3, whose name {rom_file} gives); a root stays as it is.
[[nodiscard]] std::filesystem::path tokenPath(std::filesystem::path path) {
    path.make_preferred();
    if (!path.has_filename() && path.has_relative_path()) {
        path = path.parent_path();
    }
    return path;
}

/// Expands one element and the variables it uses, keeping the chain of variables being expanded (cycles, messages) and
/// the work done: every token counts against kMaxTemplateTokens and the result never grows past kMaxExpandedBytes, so
/// that variables used over and over (each value using the next variable twice doubles the work with every level)
/// fail at once instead of taking hours and gigabytes.
class Expander {
public:
    Expander(const TemplateContext& context, bool checkOnly) : m_context(context), m_checkOnly(checkOnly) {
        // The paths of the tokens, prepared once rather than for each of the tokens.
        if (context.rom) {
            m_rom = tokenPath(*context.rom);
        }
        if (context.emulatorDirectory) {
            m_emulatorDirectory = tokenPath(*context.emulatorDirectory);
        }
    }

    /// Appends the expansion of `element` to `out`.
    [[nodiscard]] Status expand(std::string& out, std::string_view element) {
        auto parts = parseTemplateElement(element);
        if (!parts) {
            return propagate(std::move(parts.error()));
        }
        for (const TemplatePart& part : *parts) {
            if (auto expanded = expandPart(out, part, element); !expanded) {
                return expanded;
            }
        }
        if (out.find('\0') != std::string::npos) {
            return failure(ErrorCode::ValidationFailed, "the element expands to text holding a NUL character",
                           element);
        }
        if (!core::isValidUtf8(out)) {
            return failure(ErrorCode::ValidationFailed, "the element expands to text that is not valid UTF-8",
                           element);
        }
        return ok();
    }

private:
    /// " (in {var:outer})" while a variable's value is being expanded.
    [[nodiscard]] std::string where() const {
        return m_stack.empty() ? std::string() : fmt::format(" (in {{var:{}}})", m_stack.back());
    }

    [[nodiscard]] const std::string* findVariable(std::string_view name) const {
        for (const TemplateVariables* scope : m_context.variables) {
            if (scope == nullptr) {
                continue;
            }
            if (const auto found = scope->find(name); found != scope->end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    /// Appends `text` unless the result would pass kMaxExpandedBytes.
    [[nodiscard]] Status append(std::string& out, std::string_view text, std::string_view element) {
        if (text.size() > kMaxExpandedBytes - out.size()) { // out.size() never passes the limit
            return failure(ErrorCode::ValidationFailed,
                           fmt::format("the element expands to more than {} bytes, longer than a command line, a path "
                                       "or an environment value can be{}",
                                       kMaxExpandedBytes, where()),
                           element);
        }
        out += text;
        return ok();
    }

    [[nodiscard]] Status appendPath(std::string& out, const std::filesystem::path& path, std::string_view what,
                                    std::string_view element) {
        auto utf8 = core::tryPathToUtf8(path);
        if (!utf8) {
            return failure(ErrorCode::ValidationFailed, fmt::format("{} cannot be represented in UTF-8", what),
                           element);
        }
        return append(out, *utf8, element);
    }

    [[nodiscard]] Status expandVariable(std::string& out, const TemplatePart& part, std::string_view element) {
        const std::string& name = part.text;
        const std::string* value = findVariable(name);
        if (value == nullptr) {
            return failure(ErrorCode::NotFound, fmt::format("unknown variable {{var:{}}}{}", name, where()), element);
        }
        if (std::find(m_stack.begin(), m_stack.end(), name) != m_stack.end()) {
            std::string chain;
            for (const std::string& outer : m_stack) {
                chain += fmt::format("{{var:{}}} -> ", outer);
            }
            return failure(ErrorCode::ValidationFailed,
                           fmt::format("variables refer to each other: {}{{var:{}}}", chain, name), element);
        }
        if (m_stack.size() >= kMaxNesting) {
            return failure(ErrorCode::ValidationFailed,
                           fmt::format("variables nest deeper than {} levels at {{var:{}}}", kMaxNesting, name),
                           element);
        }
        auto parts = parseTemplateElement(*value);
        if (!parts) {
            return failure(parts.error().code, fmt::format("in {{var:{}}}: {}", name, parts.error().message),
                           element);
        }
        m_stack.push_back(name);
        for (const TemplatePart& inner : *parts) {
            if (auto expanded = expandPart(out, inner, element); !expanded) {
                return expanded;
            }
        }
        m_stack.pop_back();
        return ok();
    }

    [[nodiscard]] Status expandEnvironment(std::string& out, const TemplatePart& part, std::string_view element) {
        const std::string& name = part.text;
        // Looked up by the allow-list's spelling: names ignore case, as in Windows.
        const auto allowed = std::find_if(kEnvironmentAllowList.begin(), kEnvironmentAllowList.end(),
                                          [&](std::string_view entry) { return sameVariableName(entry, name); });
        if (allowed == kEnvironmentAllowList.end()) {
            return failure(ErrorCode::ValidationFailed,
                           fmt::format("{{env:{}}}{}: templates read only the environment variables that name folders "
                                       "({})",
                                       name, where(), fmt::join(kEnvironmentAllowList, ", ")),
                           element);
        }
        if (!m_context.environment) {
            return failure(ErrorCode::InvalidArgument, fmt::format("{{env:{}}}{} has no value here", name, where()),
                           element);
        }
        const auto value = m_context.environment(*allowed);
        if (!value) {
            return failure(ErrorCode::NotFound,
                           fmt::format("the environment variable {{env:{}}}{} is not set", name, where()), element);
        }
        return append(out, *value, element);
    }

    [[nodiscard]] Status expandPart(std::string& out, const TemplatePart& part, std::string_view element) {
        if (!part.token) {
            return append(out, part.text, element);
        }
        if (++m_tokens > kMaxTemplateTokens) {
            return failure(ErrorCode::ValidationFailed,
                           fmt::format("the element expands more than {} tokens, those in the values of its variables "
                                       "included (variables used over and over), at {}{}",
                                       kMaxTemplateTokens, written(part), where()),
                           element);
        }
        const auto missing = [&]() -> Status {
            if (m_checkOnly) {
                return ok(); // checked without the values of the paths
            }
            return failure(ErrorCode::InvalidArgument, fmt::format("{}{} has no value here", written(part), where()),
                           element);
        };
        switch (*part.token) {
        case TemplateToken::Rom:
        case TemplateToken::RomDir:
        case TemplateToken::RomName:
        case TemplateToken::RomFile:
        case TemplateToken::RomExt: {
            if (!m_rom) {
                return missing();
            }
            const std::filesystem::path& rom = *m_rom;
            switch (*part.token) {
            case TemplateToken::RomDir:
                return appendPath(out, rom.parent_path(), "the ROM's folder", element);
            case TemplateToken::RomName:
                return appendPath(out, rom.stem(), "the ROM's name", element);
            case TemplateToken::RomFile:
                return appendPath(out, rom.filename(), "the ROM's name", element);
            case TemplateToken::RomExt:
                return appendPath(out, rom.extension(), "the ROM's extension", element);
            default:
                return appendPath(out, rom, "the ROM's path", element);
            }
        }
        case TemplateToken::EmulatorDir:
            if (!m_emulatorDirectory) {
                return missing();
            }
            return appendPath(out, *m_emulatorDirectory, "the emulator's folder", element);
        case TemplateToken::System:
            if (!m_context.system) {
                return missing();
            }
            return append(out, *m_context.system, element);
        case TemplateToken::Variable:
            return expandVariable(out, part, element);
        case TemplateToken::Environment:
            return expandEnvironment(out, part, element);
        }
        return failure(ErrorCode::ParseError, "unknown token", element);
    }

    const TemplateContext& m_context;
    bool m_checkOnly;
    std::optional<std::filesystem::path> m_rom;
    std::optional<std::filesystem::path> m_emulatorDirectory;
    std::vector<std::string> m_stack;
    std::size_t m_tokens = 0; ///< tokens expanded so far, at every level
};

} // namespace

std::string_view toString(TemplateToken token) noexcept {
    switch (token) {
    case TemplateToken::Variable:
        return "var";
    case TemplateToken::Environment:
        return "env";
    default:
        break;
    }
    for (const NamedToken& plain : kPlainTokens) {
        if (plain.token == token) {
            return plain.name;
        }
    }
    return "unknown";
}

Expected<std::vector<TemplatePart>> parseTemplateElement(std::string_view element) {
    std::vector<TemplatePart> parts;
    std::string literal;
    const auto flush = [&] {
        if (!literal.empty()) {
            parts.push_back(TemplatePart{std::nullopt, std::move(literal)});
            literal.clear();
        }
    };
    for (std::size_t i = 0; i < element.size(); ++i) {
        const char c = element[i];
        const bool doubled = i + 1 < element.size() && element[i + 1] == c;
        if (c == '}') {
            if (!doubled) {
                return failure(ErrorCode::ParseError, "'}' without its '{' (a literal brace is written \"}}\")",
                               element);
            }
            literal.push_back('}');
            ++i;
            continue;
        }
        if (c != '{') {
            literal.push_back(c);
            continue;
        }
        if (doubled) {
            literal.push_back('{');
            ++i;
            continue;
        }
        const std::size_t close = element.find_first_of("{}", i + 1);
        if (close == std::string_view::npos || element[close] == '{') {
            return failure(ErrorCode::ParseError, "'{' without its '}' (a literal brace is written \"{{\")", element);
        }
        auto part = parseToken(element.substr(i + 1, close - i - 1), element);
        if (!part) {
            return propagate(std::move(part.error()));
        }
        flush();
        parts.push_back(std::move(*part));
        i = close;
    }
    flush();
    return parts;
}

bool isValidVariableName(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxVariableNameLength) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == '.';
    });
}

std::span<const std::string_view> templateEnvironmentAllowList() noexcept {
    return kEnvironmentAllowList;
}

bool isAllowedEnvironmentVariable(std::string_view name) noexcept {
    return std::any_of(kEnvironmentAllowList.begin(), kEnvironmentAllowList.end(),
                       [&](std::string_view allowed) { return sameVariableName(allowed, name); });
}

EnvironmentLookup processEnvironmentLookup() {
    return [](std::string_view name) -> std::optional<std::string> {
        auto value = processEnvironmentVariable(name);
        if (!value || value->empty()) {
            return std::nullopt;
        }
        return value;
    };
}

Expected<std::string> expandTemplateElement(std::string_view element, const TemplateContext& context) {
    std::string out;
    Expander expander(context, /*checkOnly*/ false);
    if (auto expanded = expander.expand(out, element); !expanded) {
        return propagate(std::move(expanded.error()));
    }
    return out;
}

Expected<std::vector<std::string>> expandTemplate(std::span<const std::string> elements,
                                                  const TemplateContext& context) {
    std::vector<std::string> arguments;
    arguments.reserve(elements.size());
    for (std::size_t i = 0; i < elements.size(); ++i) {
        auto argument = expandTemplateElement(elements[i], context);
        if (!argument) {
            Error error = std::move(argument.error());
            error.context = fmt::format("[{}] {}", i, elements[i]);
            return propagate(std::move(error));
        }
        arguments.push_back(std::move(*argument));
    }
    return arguments;
}

Status checkTemplateElement(std::string_view element, const TemplateContext& context) {
    std::string out;
    Expander expander(context, /*checkOnly*/ true);
    return expander.expand(out, element);
}

} // namespace rv::launcher
