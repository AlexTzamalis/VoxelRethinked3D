#include "vxe/io/compression.hpp"

#include <lz4.h>
#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <memory>

namespace vxe::io {
namespace {

// zstd contexts are a few hundred KB of state; creating one per call would
// dominate small compressions. One per thread is reused and freed when the
// thread exits.
struct ZstdCompressContext {
    ZSTD_CCtx* context = ZSTD_createCCtx();
    ZstdCompressContext() = default;
    ZstdCompressContext(const ZstdCompressContext&) = delete;
    ZstdCompressContext& operator=(const ZstdCompressContext&) = delete;
    ZstdCompressContext(ZstdCompressContext&&) = delete;
    ZstdCompressContext& operator=(ZstdCompressContext&&) = delete;
    ~ZstdCompressContext() { ZSTD_freeCCtx(context); }
};

struct ZstdDecompressContext {
    ZSTD_DCtx* context = ZSTD_createDCtx();
    ZstdDecompressContext() = default;
    ZstdDecompressContext(const ZstdDecompressContext&) = delete;
    ZstdDecompressContext& operator=(const ZstdDecompressContext&) = delete;
    ZstdDecompressContext(ZstdDecompressContext&&) = delete;
    ZstdDecompressContext& operator=(ZstdDecompressContext&&) = delete;
    ~ZstdDecompressContext() { ZSTD_freeDCtx(context); }
};

ZSTD_CCtx* zstdCompressContext() {
    thread_local const ZstdCompressContext kHolder;
    return kHolder.context;
}

ZSTD_DCtx* zstdDecompressContext() {
    thread_local const ZstdDecompressContext kHolder;
    return kHolder.context;
}

// LZ4 takes int sizes. Larger inputs are an error, not a silent truncation.
bool fitsLz4(usize size) noexcept {
    return size <= static_cast<usize>(LZ4_MAX_INPUT_SIZE);
}

Error codecError(std::string_view what, std::string_view detail) {
    return Error(ErrorCode::IoError, std::format("{}: {}", what, detail));
}

} // namespace

usize maxCompressedSize(usize sourceSize, Codec codec) noexcept {
    switch (codec) {
    case Codec::None:
        return sourceSize;
    case Codec::Zstd:
        return ZSTD_compressBound(sourceSize);
    case Codec::Lz4:
        return fitsLz4(sourceSize) ? static_cast<usize>(LZ4_compressBound(static_cast<int>(sourceSize))) : 0;
    }
    return 0;
}

Result<usize> compressInto(std::span<const std::byte> source, std::span<std::byte> destination, Codec codec,
                           int level) {
    switch (codec) {
    case Codec::None:
        if (destination.size() < source.size()) {
            return Error(ErrorCode::InvalidArgument, "Destination too small for uncompressed data");
        }
        std::ranges::copy(source, destination.begin());
        return source.size();

    case Codec::Zstd: {
        const int zstdLevel = level == 0 ? kDefaultZstdLevel : std::clamp(level, 1, ZSTD_maxCLevel());
        const usize written = ZSTD_compressCCtx(zstdCompressContext(), destination.data(), destination.size(),
                                                source.data(), source.size(), zstdLevel);
        if (ZSTD_isError(written) != 0U) {
            return codecError("zstd compression failed", ZSTD_getErrorName(written));
        }
        return written;
    }

    case Codec::Lz4: {
        if (!fitsLz4(source.size())) {
            return Error(ErrorCode::InvalidArgument, "Input too large for LZ4");
        }
        const int acceleration = level == 0 ? kDefaultLz4Acceleration : std::max(level, 1);
        const int capacity =
            static_cast<int>(std::min<usize>(destination.size(), std::numeric_limits<int>::max()));
        const int written = LZ4_compress_fast(reinterpret_cast<const char*>(source.data()),
                                              reinterpret_cast<char*>(destination.data()),
                                              static_cast<int>(source.size()), capacity, acceleration);
        if (written <= 0 && !source.empty()) {
            return codecError("LZ4 compression failed", "destination too small");
        }
        return static_cast<usize>(written);
    }
    }
    return Error(ErrorCode::InvalidArgument, "Unknown codec");
}

Result<void> decompressInto(std::span<const std::byte> source, std::span<std::byte> destination,
                            Codec codec) {
    switch (codec) {
    case Codec::None:
        if (source.size() != destination.size()) {
            return Error(ErrorCode::ParseError, "Uncompressed size does not match");
        }
        std::ranges::copy(source, destination.begin());
        return {};

    case Codec::Zstd: {
        const usize written = ZSTD_decompressDCtx(zstdDecompressContext(), destination.data(),
                                                  destination.size(), source.data(), source.size());
        if (ZSTD_isError(written) != 0U) {
            return Error(ErrorCode::ParseError,
                         std::format("zstd decompression failed: {}", ZSTD_getErrorName(written)));
        }
        if (written != destination.size()) {
            return Error(ErrorCode::ParseError,
                         std::format("zstd data has {} bytes, expected {}", written, destination.size()));
        }
        return {};
    }

    case Codec::Lz4: {
        if (!fitsLz4(source.size()) || !fitsLz4(destination.size())) {
            return Error(ErrorCode::InvalidArgument, "Data too large for LZ4");
        }
        // The _safe variant never reads past `source` or writes past
        // `destination`, even for corrupt or hostile input.
        const int written = LZ4_decompress_safe(
            reinterpret_cast<const char*>(source.data()), reinterpret_cast<char*>(destination.data()),
            static_cast<int>(source.size()), static_cast<int>(destination.size()));
        if (written < 0) {
            return Error(ErrorCode::ParseError, "LZ4 data is corrupt");
        }
        if (static_cast<usize>(written) != destination.size()) {
            return Error(ErrorCode::ParseError,
                         std::format("LZ4 data has {} bytes, expected {}", written, destination.size()));
        }
        return {};
    }
    }
    return Error(ErrorCode::InvalidArgument, "Unknown codec");
}

Result<std::vector<std::byte>> compress(std::span<const std::byte> source, Codec codec, int level) {
    std::vector<std::byte> output(std::max<usize>(maxCompressedSize(source.size(), codec), 1));
    Result<usize> written = compressInto(source, output, codec, level);
    if (!written) {
        return std::move(written).error();
    }
    output.resize(written.value());
    return output;
}

Result<std::vector<std::byte>> decompress(std::span<const std::byte> source, Codec codec,
                                          usize originalSize) {
    std::vector<std::byte> output(originalSize);
    Result<void> result = decompressInto(source, output, codec);
    if (!result) {
        return std::move(result).error();
    }
    return output;
}

} // namespace vxe::io
