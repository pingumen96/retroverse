#pragma once

#include <string>
#include <string_view>
#include <utility>

#include <tl/expected.hpp>

namespace rv {

/// Broad error categories: callers branch on the code, humans read the message.
enum class ErrorCode {
    Unknown,
    InvalidArgument,
    NotFound,
    IoError,
    ParseError,
    Unsupported,
    OutOfRange,
    ValidationFailed,
    GraphicsError,
    PlatformError,
};

[[nodiscard]] std::string_view toString(ErrorCode code) noexcept;

/// Error value carried by rv::Expected. `context` names the subject (file path, asset id, JSON path...).
struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string message;
    std::string context;

    /// Human readable form: "<code>: <message> [<context>]".
    [[nodiscard]] std::string toString() const;
};

/// Value-or-error return type used across Retroverse (std::expected is C++23; tl::expected has the same API).
template <class T>
using Expected = tl::expected<T, Error>;

/// Result of an operation that produces no value.
using Status = Expected<void>;

/// Builds the error branch: `return rv::makeError(rv::ErrorCode::NotFound, "file missing", path);`
[[nodiscard]] inline tl::unexpected<Error> makeError(ErrorCode code, std::string message, std::string context = {}) {
    return tl::unexpected<Error>(Error{code, std::move(message), std::move(context)});
}

/// Re-wraps an existing error, e.g. when propagating from a nested call.
[[nodiscard]] inline tl::unexpected<Error> propagate(Error error) {
    return tl::unexpected<Error>(std::move(error));
}

/// Successful Status.
[[nodiscard]] inline Status ok() {
    return {};
}

} // namespace rv
