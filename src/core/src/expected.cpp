#include <rv/core/expected.hpp>

namespace rv {

std::string_view toString(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Unknown:
        return "unknown";
    case ErrorCode::InvalidArgument:
        return "invalid-argument";
    case ErrorCode::NotFound:
        return "not-found";
    case ErrorCode::IoError:
        return "io-error";
    case ErrorCode::ParseError:
        return "parse-error";
    case ErrorCode::Unsupported:
        return "unsupported";
    case ErrorCode::OutOfRange:
        return "out-of-range";
    case ErrorCode::ValidationFailed:
        return "validation-failed";
    case ErrorCode::GraphicsError:
        return "graphics-error";
    case ErrorCode::PlatformError:
        return "platform-error";
    case ErrorCode::Unavailable:
        return "unavailable";
    case ErrorCode::Conflict:
        return "conflict";
    }
    return "unknown";
}

std::string Error::toString() const {
    std::string out{rv::toString(code)};
    out += ": ";
    out += message;
    if (!context.empty()) {
        out += " [";
        out += context;
        out += "]";
    }
    return out;
}

} // namespace rv
