#pragma once

#include <vxe/core/types.hpp>

#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace vxe {

// Broad categories of expected failures. Callers branch on the code; the
// message is for humans (logs, error dialogs). Add codes only when some
// caller needs to tell the new case apart from the existing ones.
enum class ErrorCode : u16 {
    Unknown,
    InvalidArgument, // The caller passed something that can never work.
    NotFound,        // A file, setting, texture key or similar does not exist.
    AlreadyExists,
    PermissionDenied,
    IoError,     // Reading or writing failed for a reason outside our control.
    ParseError,  // A file exists but its contents are malformed.
    Unsupported, // Valid request, but this build, platform or GPU cannot do it.
    OutOfMemory, // A budget or allocation limit was reached.
    Cancelled,   // The work was cancelled (for example a chunk left render distance).
    Timeout,
    VersionMismatch, // Data from a newer or incompatible format version.
};

// Returns the name of a code, for example "NotFound". Not called toString on
// purpose: test frameworks (doctest) look up toString through ADL for printing.
[[nodiscard]] std::string_view nameOf(ErrorCode code) noexcept;

// An expected failure: what kind (code), what happened (message) and where it
// was created (location). Errors are values, returned through vxe::Result, and
// never thrown (Section 23.4 of the plan).
//
// The location is captured automatically where the Error is constructed, so
// `return Error(ErrorCode::NotFound, "pack.toml missing");` records that line.
class Error {
public:
    explicit Error(ErrorCode code, std::string message = {},
                   std::source_location location = std::source_location::current())
        : m_message(std::move(message))
        , m_location(location)
        , m_code(code) {}

    [[nodiscard]] ErrorCode code() const noexcept { return m_code; }
    [[nodiscard]] const std::string& message() const noexcept { return m_message; }
    [[nodiscard]] const std::source_location& location() const noexcept { return m_location; }

    // Prepends context while an error travels up, keeping code and location.
    // For example "Loading pack 'MyPack': " + "pack.toml missing".
    [[nodiscard]] Error withContext(std::string_view context) const&;
    [[nodiscard]] Error withContext(std::string_view context) &&;

    // A one line description for logs: "NotFound: pack.toml missing (pack_loader.cpp:42)".
    [[nodiscard]] std::string describe() const;

private:
    std::string m_message;
    std::source_location m_location;
    ErrorCode m_code;
};

} // namespace vxe
