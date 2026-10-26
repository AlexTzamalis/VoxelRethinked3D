#include "vxe/core/error.hpp"

#include <filesystem>
#include <format>

namespace vxe {

std::string_view nameOf(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Unknown:
        return "Unknown";
    case ErrorCode::InvalidArgument:
        return "InvalidArgument";
    case ErrorCode::NotFound:
        return "NotFound";
    case ErrorCode::AlreadyExists:
        return "AlreadyExists";
    case ErrorCode::PermissionDenied:
        return "PermissionDenied";
    case ErrorCode::IoError:
        return "IoError";
    case ErrorCode::ParseError:
        return "ParseError";
    case ErrorCode::Unsupported:
        return "Unsupported";
    case ErrorCode::OutOfMemory:
        return "OutOfMemory";
    case ErrorCode::Cancelled:
        return "Cancelled";
    case ErrorCode::Timeout:
        return "Timeout";
    case ErrorCode::VersionMismatch:
        return "VersionMismatch";
    }
    return "Unknown";
}

Error Error::withContext(std::string_view context) const& {
    Error copy = *this;
    return std::move(copy).withContext(context);
}

Error Error::withContext(std::string_view context) && {
    m_message.insert(0, std::format("{}: ", context));
    return std::move(*this);
}

std::string Error::describe() const {
    // Only the file name, not the full build machine path, keeps log lines short.
    const std::string file = std::filesystem::path(m_location.file_name()).filename().string();
    if (m_message.empty()) {
        return std::format("{} ({}:{})", nameOf(m_code), file, m_location.line());
    }
    return std::format("{}: {} ({}:{})", nameOf(m_code), m_message, file, m_location.line());
}

} // namespace vxe
