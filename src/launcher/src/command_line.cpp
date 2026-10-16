#include <rv/launcher/command_line.hpp>

#include <fmt/format.h>

namespace rv::launcher {
namespace {

/// Characters after which CommandLineToArgvW (space, tab) or other parsers (newline, vertical tab) could split an
/// argument, and the double quote, which must be escaped.
constexpr std::string_view kNeedsQuoting = " \t\n\v\"";

[[nodiscard]] bool holdsNul(std::string_view text) noexcept {
    return text.find('\0') != std::string_view::npos;
}

} // namespace

bool isValidUtf8(std::string_view text) noexcept {
    std::size_t i = 0;
    const std::size_t size = text.size();
    while (i < size) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        char32_t codePoint = 0;
        char32_t smallest = 0;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            codePoint = lead & 0x1Fu;
            smallest = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            codePoint = lead & 0x0Fu;
            smallest = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            codePoint = lead & 0x07u;
            smallest = 0x10000;
        } else {
            return false; // a continuation byte without a lead byte, or a lead byte of a 5- or 6-byte form
        }
        if (size - i < length) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto continuation = static_cast<unsigned char>(text[i + k]);
            if ((continuation & 0xC0) != 0x80) {
                return false;
            }
            codePoint = (codePoint << 6) | (continuation & 0x3Fu);
        }
        if (codePoint < smallest || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
            return false;
        }
        i += length;
    }
    return true;
}

std::size_t utf16Length(std::string_view text) noexcept {
    std::size_t units = 0;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte & 0xC0) == 0x80) {
            continue; // continuation byte
        }
        units += byte >= 0xF0 ? 2u : 1u; // 4-byte forms encode code points above U+FFFF: a surrogate pair
    }
    return units;
}

void appendQuotedArgument(std::string& commandLine, std::string_view argument) {
    if (!argument.empty() && argument.find_first_of(kNeedsQuoting) == std::string_view::npos) {
        commandLine.append(argument);
        return;
    }
    commandLine.push_back('"');
    std::size_t i = 0;
    while (true) {
        std::size_t backslashes = 0;
        while (i < argument.size() && argument[i] == '\\') {
            ++backslashes;
            ++i;
        }
        if (i == argument.size()) {
            // Doubled, so that the closing quote added below still closes the argument.
            commandLine.append(backslashes * 2, '\\');
            break;
        }
        if (argument[i] == '"') {
            // Doubled, plus one that makes the quote literal.
            commandLine.append(backslashes * 2 + 1, '\\');
            commandLine.push_back('"');
        } else {
            commandLine.append(backslashes, '\\');
            commandLine.push_back(argument[i]);
        }
        ++i;
    }
    commandLine.push_back('"');
}

std::string quoteWindowsArgument(std::string_view argument) {
    std::string quoted;
    appendQuotedArgument(quoted, argument);
    return quoted;
}

Expected<std::string> buildCommandLine(std::string_view program, std::span<const std::string> arguments) {
    if (program.empty()) {
        return makeError(ErrorCode::ValidationFailed, "the program name is empty", "argv[0]");
    }
    if (program.find('"') != std::string_view::npos) {
        return makeError(ErrorCode::ValidationFailed, "the program name holds a double quote", std::string(program));
    }
    if (holdsNul(program) || !isValidUtf8(program)) {
        return makeError(ErrorCode::ValidationFailed, "the program name holds a NUL character or malformed UTF-8",
                         "argv[0]");
    }
    std::size_t reserve = program.size() + 2;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        if (holdsNul(argument)) {
            return makeError(ErrorCode::ValidationFailed, "an argument holds a NUL character",
                             fmt::format("argv[{}]", i + 1));
        }
        if (!isValidUtf8(argument)) {
            return makeError(ErrorCode::ValidationFailed, "an argument is not valid UTF-8",
                             fmt::format("argv[{}]", i + 1));
        }
        reserve += argument.size() + 3;
    }

    std::string commandLine;
    commandLine.reserve(reserve);
    commandLine.push_back('"');
    commandLine.append(program);
    commandLine.push_back('"');
    for (const std::string& argument : arguments) {
        commandLine.push_back(' ');
        appendQuotedArgument(commandLine, argument);
    }
    if (const std::size_t units = utf16Length(commandLine); units > kMaxCommandLineUnits) {
        return makeError(ErrorCode::ValidationFailed,
                         fmt::format("the command line is {} UTF-16 code units long; Windows accepts at most {}",
                                     units, kMaxCommandLineUnits),
                         std::string(program));
    }
    return commandLine;
}

} // namespace rv::launcher
