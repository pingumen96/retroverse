#include <rv/assets/shader_source.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <new>
#include <optional>
#include <set>
#include <utility>

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>

#include "path_util.hpp"

namespace rv::assets {
namespace {

constexpr std::uintmax_t kMaxShaderFileBytes = 4ull * 1024ull * 1024ull;
constexpr std::size_t kMaxIncludeDepth = 32;
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool isIdentifierStart(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool isIdentifierChar(char c) noexcept {
    return isIdentifierStart(c) || isDigit(c);
}

bool isBlank(char c) noexcept {
    return c == ' ' || c == '\t';
}

std::string genericUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

/// Splits on LF, dropping a trailing CR from each line; a final newline does not produce an extra empty line.
std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        start = end + 1;
    }
    return lines;
}

/// Returns the code part of one line (comments replaced by a space), tracking block comments across lines.
std::string stripComments(std::string_view line, bool& inBlockComment) {
    std::string code;
    code.reserve(line.size());
    std::size_t i = 0;
    while (i < line.size()) {
        if (inBlockComment) {
            const std::size_t end = line.find("*/", i);
            if (end == std::string_view::npos) {
                return code;
            }
            inBlockComment = false;
            code += ' ';
            i = end + 2;
            continue;
        }
        if (line[i] == '/' && i + 1 < line.size()) {
            if (line[i + 1] == '/') {
                break;
            }
            if (line[i + 1] == '*') {
                inBlockComment = true;
                i += 2;
                continue;
            }
        }
        code += line[i];
        ++i;
    }
    return code;
}

struct Directive {
    std::string_view name;
    std::string_view argument;
};

/// Recognizes "#name rest" at the start of a comment-free line. The views point into `code`.
std::optional<Directive> parseDirective(std::string_view code) {
    std::size_t i = 0;
    while (i < code.size() && isBlank(code[i])) {
        ++i;
    }
    if (i >= code.size() || code[i] != '#') {
        return std::nullopt;
    }
    ++i;
    while (i < code.size() && isBlank(code[i])) {
        ++i;
    }
    const std::size_t nameStart = i;
    while (i < code.size() && isIdentifierChar(code[i])) {
        ++i;
    }
    return Directive{code.substr(nameStart, i - nameStart), code.substr(i)};
}

Expected<std::string> parseIncludeArgument(std::string_view argument, const std::string& location) {
    const std::string_view text = core::trim(argument);
    if (text.empty()) {
        return makeError(ErrorCode::ParseError, "#include needs a quoted path", location);
    }
    if (text.front() == '<') {
        return makeError(ErrorCode::ParseError, "#include <...> is not supported; use #include \"path\"", location);
    }
    if (text.front() != '"') {
        return makeError(ErrorCode::ParseError, "malformed #include: expected a quoted path", location);
    }
    const std::size_t close = text.find('"', 1);
    if (close == std::string_view::npos) {
        return makeError(ErrorCode::ParseError, "malformed #include: missing closing quote", location);
    }
    if (!core::trim(text.substr(close + 1)).empty()) {
        return makeError(ErrorCode::ParseError, "malformed #include: unexpected text after the path", location);
    }
    return std::string(text.substr(1, close - 1));
}

Expected<std::string> formatDefine(std::string_view define) {
    const std::size_t equals = define.find('=');
    const std::string_view name = core::trim(define.substr(0, equals));
    const std::string_view value =
        equals == std::string_view::npos ? std::string_view{} : core::trim(define.substr(equals + 1));
    const std::string context = "define \"" + std::string(define) + "\"";
    if (name.empty() || !isIdentifierStart(name.front()) ||
        !std::all_of(name.begin(), name.end(), [](char c) { return isIdentifierChar(c); })) {
        return makeError(ErrorCode::InvalidArgument, "macro name must be an identifier", context);
    }
    if (name.starts_with("GL_") || name.find("__") != std::string_view::npos) {
        return makeError(ErrorCode::InvalidArgument, "macro names starting with GL_ or containing __ are reserved",
                         context);
    }
    if (value.find_first_of("\r\n") != std::string_view::npos) {
        return makeError(ErrorCode::InvalidArgument, "macro value must fit on one line", context);
    }
    std::string line = "#define " + std::string(name);
    if (!value.empty()) {
        line += ' ';
        line += value;
    }
    return line;
}

class Preprocessor {
public:
    Preprocessor(const std::filesystem::path& root, std::vector<std::filesystem::path>& dependencies)
        : m_root(root), m_dependencies(dependencies) {}

    Expected<ShaderSource> run(std::string_view rootPath, std::span<const std::string> defines) {
        for (const std::string& define : defines) {
            auto line = formatDefine(define);
            if (!line) {
                return propagate(line.error());
            }
            m_defineLines.push_back(std::move(*line));
        }
        auto path = detail::normalizeRelativePath(rootPath, false, rootPath);
        if (!path) {
            return propagate(path.error());
        }
        auto text = readSource(*path, *path);
        if (!text) {
            return propagate(text.error());
        }
        m_source.files.push_back(core::pathFromUtf8(*path));
        m_included.insert(core::toLowerAscii(*path));
        m_stack.push_back(*path);
        if (auto status = expand(*path, *text, 0); !status) {
            return propagate(status.error());
        }
        return std::move(m_source);
    }

private:
    /// Reads one file of the load. Every attempt is a dependency, a failed one too: creating or fixing that file
    /// changes the outcome.
    Expected<std::string> readSource(const std::string& relative, const std::string& context) {
        m_dependencies.push_back(core::pathFromUtf8(relative));
        auto text = core::readTextFile(m_root / m_dependencies.back(), kMaxShaderFileBytes);
        if (!text) {
            Error error = text.error();
            error.message = "cannot read \"" + relative + "\": " + error.message;
            error.context = context;
            return propagate(std::move(error));
        }
        if (text->starts_with(kUtf8Bom)) {
            text->erase(0, kUtf8Bom.size());
        }
        return text;
    }

    void emit(std::string_view line) {
        m_source.text.append(line);
        m_source.text.push_back('\n');
    }

    [[nodiscard]] std::string includeChain(const std::string& next) const {
        std::string chain;
        for (const std::string& file : m_stack) {
            chain += file;
            chain += " -> ";
        }
        return chain + next;
    }

    Status expand(const std::string& relative, std::string_view text, std::size_t fileIndex) {
        const bool isRoot = fileIndex == 0;
        bool versionSeen = !isRoot;
        bool inBlockComment = false;
        const std::vector<std::string_view> lines = splitLines(text);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::size_t lineNumber = i + 1;
            const std::string location = relative + ":" + std::to_string(lineNumber);
            const bool startsInBlockComment = inBlockComment;
            const std::string code = stripComments(lines[i], inBlockComment);
            const std::optional<Directive> directive = parseDirective(code);

            if (!versionSeen) {
                if (core::trim(code).empty()) {
                    emit(lines[i]);
                    continue;
                }
                if (!directive || directive->name != "version") {
                    return makeError(ErrorCode::ParseError,
                                     "the root shader must start with a #version directive (only comments and "
                                     "blank lines may precede it)",
                                     location);
                }
                if (inBlockComment) { // the injected defines and #line would be commented out
                    return makeError(ErrorCode::ParseError, "a block comment must not start on the #version line",
                                     location);
                }
                emit(lines[i]);
                versionSeen = true;
                for (const std::string& define : m_defineLines) {
                    emit(define);
                }
                if (!m_defineLines.empty()) {
                    emit("#line " + std::to_string(lineNumber + 1) + " " + std::to_string(fileIndex));
                }
                continue;
            }
            if (directive && directive->name == "version") {
                return makeError(ErrorCode::ParseError,
                                 isRoot ? "#version must appear only once, at the top of the root shader"
                                        : "#version is only allowed in the root shader, not in included files",
                                 location);
            }
            if (!directive || directive->name != "include") {
                emit(lines[i]);
                continue;
            }
            if (startsInBlockComment || inBlockComment) { // the include line is replaced, comment delimiters included
                return makeError(ErrorCode::ParseError, "a block comment must not start or end on an #include line",
                                 location);
            }
            if (auto status = include(directive->argument, location, lineNumber, fileIndex); !status) {
                return status;
            }
        }
        if (!versionSeen) {
            return makeError(ErrorCode::ParseError, "no #version directive found", relative);
        }
        if (inBlockComment && !isRoot) {
            return makeError(ErrorCode::ParseError, "unterminated block comment at the end of an included file",
                             relative);
        }
        return ok();
    }

    Status include(std::string_view argument, const std::string& location, std::size_t lineNumber,
                   std::size_t parentIndex) {
        auto requested = parseIncludeArgument(argument, location);
        if (!requested) {
            return propagate(requested.error());
        }
        auto path = detail::normalizeRelativePath(*requested, false, location);
        if (!path) {
            return propagate(path.error());
        }
        // Windows file names ignore letter case: "common/BRDF.glsl" and "common/brdf.glsl" are the same file.
        const auto sameFile = [&path](const std::string& other) { return core::equalsIgnoreCaseAscii(other, *path); };
        if (std::any_of(m_stack.begin(), m_stack.end(), sameFile)) {
            return makeError(ErrorCode::ParseError, "include cycle: " + includeChain(*path), location);
        }
        const std::string key = core::toLowerAscii(*path);
        if (m_included.contains(key)) {
            emit(""); // already expanded once in this load; keep the line numbering of the parent
            return ok();
        }
        if (m_stack.size() >= kMaxIncludeDepth) {
            return makeError(ErrorCode::OutOfRange,
                             "includes are nested deeper than " + std::to_string(kMaxIncludeDepth) + " levels",
                             location);
        }
        auto text = readSource(*path, location);
        if (!text) {
            return propagate(text.error());
        }
        const std::size_t includedIndex = m_source.files.size();
        m_source.files.push_back(core::pathFromUtf8(*path));
        m_included.insert(key);
        m_stack.push_back(*path);
        emit("#line 1 " + std::to_string(includedIndex));
        if (auto status = expand(*path, *text, includedIndex); !status) {
            return status;
        }
        m_stack.pop_back();
        emit("#line " + std::to_string(lineNumber + 1) + " " + std::to_string(parentIndex));
        return ok();
    }

    const std::filesystem::path& m_root;
    std::vector<std::filesystem::path>& m_dependencies; // every file opened or tried, in order
    std::vector<std::string> m_defineLines;
    ShaderSource m_source;
    std::vector<std::string> m_stack;              // files being expanded, root first (as spelled)
    std::set<std::string, std::less<>> m_included; // ASCII-lowercase paths of every expanded file
};

bool parseNumber(std::string_view text, std::size_t& pos, std::size_t& value) {
    const std::size_t start = pos;
    value = 0;
    while (pos < text.size() && isDigit(text[pos])) {
        if (value > 100000000) {
            return false;
        }
        value = value * 10 + static_cast<std::size_t>(text[pos] - '0');
        ++pos;
    }
    return pos > start;
}

bool consume(std::string_view text, std::size_t& pos, std::string_view expected) {
    if (text.substr(pos).starts_with(expected)) {
        pos += expected.size();
        return true;
    }
    return false;
}

std::optional<std::string> rewriteLogLine(std::string_view line, const ShaderSource& source) {
    std::size_t pos = 0;
    std::size_t file = 0;
    std::size_t number = 0;
    const auto fileName = [&source](std::size_t index) -> std::optional<std::string> {
        if (index >= source.files.size()) {
            return std::nullopt;
        }
        return genericUtf8(source.files[index]);
    };

    if (!line.empty() && isDigit(line.front())) {
        if (!parseNumber(line, pos, file)) {
            return std::nullopt;
        }
        if (consume(line, pos, "(")) { // NVIDIA: 0(12) : error C0000: ...
            if (!parseNumber(line, pos, number) || !consume(line, pos, ") : ")) {
                return std::nullopt;
            }
            const auto name = fileName(file);
            if (!name) {
                return std::nullopt;
            }
            return *name + ":" + std::to_string(number) + ": " + std::string(line.substr(pos));
        }
        if (consume(line, pos, ":")) { // Mesa: 0:12(5): error: ...
            std::size_t column = 0;
            if (!parseNumber(line, pos, number) || !consume(line, pos, "(") || !parseNumber(line, pos, column) ||
                !consume(line, pos, "): ")) {
                return std::nullopt;
            }
            const auto name = fileName(file);
            if (!name) {
                return std::nullopt;
            }
            return *name + ":" + std::to_string(number) + ":" + std::to_string(column) + ": " +
                   std::string(line.substr(pos));
        }
        return std::nullopt;
    }

    // AMD / Intel: ERROR: 0:12: message, WARNING: 0:12: message
    std::string_view severity;
    if (consume(line, pos, "ERROR: ")) {
        severity = "error";
    } else if (consume(line, pos, "WARNING: ")) {
        severity = "warning";
    } else {
        return std::nullopt;
    }
    if (!parseNumber(line, pos, file) || !consume(line, pos, ":") || !parseNumber(line, pos, number) ||
        !consume(line, pos, ": ")) {
        return std::nullopt;
    }
    const auto name = fileName(file);
    if (!name) {
        return std::nullopt;
    }
    return *name + ":" + std::to_string(number) + ": " + std::string(severity) + ": " + std::string(line.substr(pos));
}

} // namespace

ShaderSourceLoader::ShaderSourceLoader(std::filesystem::path shaderRoot) : m_root(std::move(shaderRoot)) {}

Expected<ShaderSource> ShaderSourceLoader::load(std::string_view relativePath,
                                                std::span<const std::string> defines) const {
    std::vector<std::filesystem::path> dependencies;
    return load(relativePath, defines, dependencies);
}

Expected<ShaderSource> ShaderSourceLoader::load(std::string_view relativePath, std::span<const std::string> defines,
                                                std::vector<std::filesystem::path>& dependencies) const {
    dependencies.clear();
    try {
        Preprocessor preprocessor(m_root, dependencies);
        return preprocessor.run(relativePath, defines);
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfRange, "out of memory while preprocessing the shader",
                         std::string(relativePath));
    } catch (const std::exception& e) { // e.g. std::filesystem conversions of a shader root without a UTF-8 form
        return makeError(ErrorCode::Unknown, std::string("unexpected failure while preprocessing the shader: ") +
                                                 e.what(),
                         std::string(relativePath));
    }
}

std::string mapCompilerLog(std::string_view log, const ShaderSource& source) {
    std::string result;
    result.reserve(log.size() + 64);
    std::size_t start = 0;
    while (start < log.size()) {
        std::size_t end = log.find('\n', start);
        const bool hasNewline = end != std::string_view::npos;
        if (!hasNewline) {
            end = log.size();
        }
        std::string_view line = log.substr(start, end - start);
        const bool hasCarriageReturn = !line.empty() && line.back() == '\r';
        if (hasCarriageReturn) {
            line.remove_suffix(1);
        }
        if (const auto rewritten = rewriteLogLine(line, source)) {
            result += *rewritten;
        } else {
            result += line;
        }
        if (hasCarriageReturn) {
            result += '\r';
        }
        if (hasNewline) {
            result += '\n';
        }
        start = end + 1;
    }
    return result;
}

} // namespace rv::assets
