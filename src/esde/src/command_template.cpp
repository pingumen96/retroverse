#include <rv/esde/command_template.hpp>

#include <array>
#include <utility>

namespace rv::esde {
namespace {

/// What separates the elements after the first one.
bool isSeparator(char c) noexcept {
    return c == ' ' || c == '\t';
}

/// What ES-DE trims from the start of a command once it removed a flag (std::isspace in the C locale).
bool isTrimmed(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/// The first element of `line` (not empty) by the program-name rule; `pos` ends after it: after its closing quote, or
/// after the one character from U+0001 to U+0020 that ended it.
std::string programElement(std::string_view line, std::size_t& pos) {
    if (line.front() == '"') {
        const std::size_t close = line.find('"', 1);
        if (close == std::string_view::npos) {
            pos = line.size();
            return std::string(line.substr(1));
        }
        pos = close + 1;
        return std::string(line.substr(1, close - 1));
    }
    pos = 0;
    while (pos < line.size() && static_cast<unsigned char>(line[pos]) > 0x20) {
        ++pos;
    }
    std::string element(line.substr(0, pos));
    if (pos < line.size()) {
        ++pos;
    }
    return element;
}

/// One element by the rule of the elements after the first, from `pos` (on no separator); `pos` ends on the separator
/// that ended it, or at the end. The rule of shell32's CommandLineToArgvW (as Wine documents and implements it).
std::string laterElement(std::string_view line, std::size_t& pos) {
    std::string element;
    int quotes = 0;              // 0 outside a quoted part, 1 inside; the run of consecutive quotes counts modulo 3
    std::size_t backslashes = 0; // the backslashes just copied, right before the current character
    while (pos < line.size()) {
        const char c = line[pos];
        if (isSeparator(c) && quotes == 0) {
            break;
        }
        if (c == '\\') {
            element += c;
            ++backslashes;
            ++pos;
            continue;
        }
        if (c == '"') {
            if (backslashes % 2 == 0) {
                // 2n backslashes: n of them, and a quote that opens or closes a quoted part.
                element.resize(element.size() - backslashes / 2);
                ++quotes;
            } else {
                // 2n+1 backslashes: n of them and a literal quote.
                element.resize(element.size() - backslashes / 2 - 1);
                element += '"';
            }
            backslashes = 0;
            ++pos;
            // Consecutive quotes: every third gives a literal quote.
            while (pos < line.size() && line[pos] == '"') {
                if (++quotes == 3) {
                    element += '"';
                    quotes = 0;
                }
                ++pos;
            }
            if (quotes == 2) {
                quotes = 0;
            }
            continue;
        }
        element += c;
        backslashes = 0;
        ++pos;
    }
    return element;
}

/// Appends the elements of `line` from `pos` on, by the rule of the elements after the first.
void appendLaterElements(std::string_view line, std::size_t pos, std::vector<std::string>& elements) {
    while (true) {
        while (pos < line.size() && isSeparator(line[pos])) {
            ++pos;
        }
        if (pos >= line.size()) {
            return;
        }
        elements.push_back(laterElement(line, pos));
    }
}

struct KnownToken {
    std::string_view name;
    CommandTokenKind kind;
};

/// The tokens ES-DE 3.4.1 handles on Windows, matched as ES-DE matches them (std::string::find of the whole token).
constexpr std::array<KnownToken, 16> kKnownTokens{{
    {"ROM", CommandTokenKind::Variable},
    {"ROMRAW", CommandTokenKind::Variable},
    {"ROMPATH", CommandTokenKind::Variable},
    {"BASENAME", CommandTokenKind::Variable},
    {"FILENAME", CommandTokenKind::Variable},
    {"GAMEDIR", CommandTokenKind::Variable},
    {"GAMEDIRRAW", CommandTokenKind::Variable},
    {"GAMEENTRYDIR", CommandTokenKind::Variable},
    {"EMUDIR", CommandTokenKind::Variable},
    {"ESPATH", CommandTokenKind::Variable},
    {"EMUPATH", CommandTokenKind::Variable},
    {"HIDEWINDOW", CommandTokenKind::Flag},
    {"ESCAPESPECIALS", CommandTokenKind::Flag},
    {"RUNINBACKGROUND", CommandTokenKind::Flag},
    {"STARTDIR", CommandTokenKind::Assignment},
    {"INJECT", CommandTokenKind::Assignment},
}};

/// The tokens that name a find rule: the name runs to the next '%'.
constexpr std::array<std::pair<std::string_view, CommandTokenKind>, 3> kNamedTokens{{
    {"EMULATOR_", CommandTokenKind::Emulator},
    {"CORE_", CommandTokenKind::Core},
    {"PRECOMMAND_", CommandTokenKind::PreCommand},
}};

bool isAsciiLetter(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/// The name of an unknown token: a letter or '_', then letters, digits and "_-!.()".
bool isUnknownTokenName(std::string_view name) noexcept {
    if (name.empty() || !(isAsciiLetter(name.front()) || name.front() == '_')) {
        return false;
    }
    for (const char c : name) {
        const bool allowed = isAsciiLetter(c) || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '!' ||
                             c == '.' || c == '(' || c == ')';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

/// The token that starts at `at` (a '%') of `element`, if any.
std::optional<CommandToken> tokenAt(std::string_view element, std::size_t at) {
    const std::string_view rest = element.substr(at + 1);
    for (const KnownToken& known : kKnownTokens) {
        if (rest.size() > known.name.size() && rest.starts_with(known.name) && rest[known.name.size()] == '%') {
            CommandToken token;
            token.kind = known.kind;
            token.offset = at;
            token.name = std::string(known.name);
            token.text = "%" + token.name + "%";
            if (known.kind == CommandTokenKind::Assignment) {
                if (rest.size() > known.name.size() + 1 && rest[known.name.size() + 1] == '=') {
                    token.text += '=';
                } else {
                    token.kind = CommandTokenKind::Malformed;
                }
            }
            return token;
        }
    }
    const std::size_t close = rest.find('%');
    if (close == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view name = rest.substr(0, close);
    for (const auto& [prefix, kind] : kNamedTokens) {
        if (name.size() > prefix.size() && name.starts_with(prefix)) {
            CommandToken token;
            token.kind = kind;
            token.offset = at;
            token.name = std::string(name);
            token.text = "%" + token.name + "%";
            token.target = std::string(name.substr(prefix.size()));
            return token;
        }
    }
    if (!isUnknownTokenName(name)) {
        return std::nullopt;
    }
    CommandToken token;
    token.kind = CommandTokenKind::Unknown;
    token.offset = at;
    token.name = std::string(name);
    token.text = "%" + token.name + "%";
    // The %NAME%= form of ES-DE's other platforms (%ANDROIDAPP%=, %ACTION%=, ...).
    if (close + 1 < rest.size() && rest[close + 1] == '=') {
        token.text += '=';
    }
    return token;
}

/// The flags and assignments ES-DE removes from the start of a command before it launches it.
bool leadsCommand(std::string_view element) noexcept {
    return element == "%HIDEWINDOW%" || element == "%ESCAPESPECIALS%" || element == "%RUNINBACKGROUND%" ||
           element.starts_with("%STARTDIR%=");
}

CommandElement elementOf(std::string text) {
    CommandElement element;
    element.tokens = scanCommandTokens(text);
    element.text = std::move(text);
    return element;
}

} // namespace

std::vector<std::string> splitCommandLine(std::string_view commandLine) {
    std::vector<std::string> elements;
    const std::string_view line = commandLine.substr(0, commandLine.find('\0'));
    if (line.empty()) {
        return elements;
    }
    std::size_t pos = 0;
    elements.push_back(programElement(line, pos));
    appendLaterElements(line, pos, elements);
    return elements;
}

std::string_view toString(CommandTokenKind kind) noexcept {
    switch (kind) {
    case CommandTokenKind::Variable:
        return "variable";
    case CommandTokenKind::Flag:
        return "flag";
    case CommandTokenKind::Assignment:
        return "assignment";
    case CommandTokenKind::Emulator:
        return "emulator";
    case CommandTokenKind::Core:
        return "core";
    case CommandTokenKind::PreCommand:
        return "pre-command";
    case CommandTokenKind::HomeFolder:
        return "home-folder";
    case CommandTokenKind::Malformed:
        return "malformed";
    case CommandTokenKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

std::vector<CommandToken> scanCommandTokens(std::string_view element) {
    std::vector<CommandToken> tokens;
    std::size_t at = 0;
    while (at < element.size()) {
        if (element[at] == '~') {
            tokens.push_back(CommandToken{CommandTokenKind::HomeFolder, at, "~", "~", {}});
            ++at;
            continue;
        }
        if (element[at] != '%') {
            ++at;
            continue;
        }
        if (auto token = tokenAt(element, at)) {
            at += token->text.size();
            tokens.push_back(std::move(*token));
        } else {
            ++at; // a stray '%': the next one may open a token
        }
    }
    return tokens;
}

CommandTemplate parseCommandTemplate(std::string_view command) {
    CommandTemplate result;
    const std::string_view line = command.substr(0, command.find('\0'));
    std::size_t pos = 0;
    // The elements ES-DE removes before it launches the command, which it then trims.
    while (true) {
        while (pos < line.size() && isTrimmed(line[pos])) {
            ++pos;
        }
        if (pos >= line.size()) {
            break;
        }
        std::size_t next = pos;
        std::string element = laterElement(line, next);
        if (!leadsCommand(element)) {
            break;
        }
        result.elements.push_back(elementOf(std::move(element)));
        pos = next;
    }
    result.programIndex = result.elements.size();
    if (pos < line.size()) {
        for (std::string& element : splitCommandLine(line.substr(pos))) {
            result.elements.push_back(elementOf(std::move(element)));
        }
    }
    return result;
}

std::string censusKey(const CommandToken& token) {
    switch (token.kind) {
    case CommandTokenKind::Emulator:
        return "%EMULATOR_X%";
    case CommandTokenKind::Core:
        return "%CORE_X%";
    case CommandTokenKind::PreCommand:
        return "%PRECOMMAND_X%";
    default:
        return token.text;
    }
}

} // namespace rv::esde
