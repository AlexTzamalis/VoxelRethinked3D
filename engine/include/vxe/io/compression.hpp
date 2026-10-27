#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>

#include <span>
#include <vector>

// Compression helpers over zstd and LZ4 (Section 4).
//
//   Zstd  Strong and still fast. Saves and region files (Section 21), where
//         size on disk matters. Level 1 to 19, default 3.
//   Lz4   Much faster, weaker. The LOD tile cache and in memory buffers, where
//         speed matters more than size. "Level" is the acceleration: 1 is the
//         default, higher is faster and larger.
//
// Neither format stores the original size here; callers keep it next to the
// data (region headers do), and decompression checks the result matches.
//
// Thread safety: every function may be called from any thread. zstd keeps
// one compression and one decompression context per thread, so repeated
// calls do not allocate contexts.
namespace vxe::io {

enum class Codec : u8 {
    None, // Stored as is. Useful to measure the effect of compression.
    Zstd,
    Lz4,
};

inline constexpr int kDefaultZstdLevel = 3;
inline constexpr int kDefaultLz4Acceleration = 1;

// The largest possible compressed size for `sourceSize` input bytes, so a
// caller can size an output buffer once and reuse it.
[[nodiscard]] usize maxCompressedSize(usize sourceSize, Codec codec) noexcept;

// Compresses into `destination` and returns the compressed size. Fails if the
// destination is too small (use maxCompressedSize) or the codec reports an error.
[[nodiscard]] Result<usize> compressInto(std::span<const std::byte> source, std::span<std::byte> destination,
                                         Codec codec, int level = 0);

// Decompresses into `destination`, which must be exactly the original size.
// Fails on corrupt data or a size mismatch, never reads or writes out of bounds.
[[nodiscard]] Result<void> decompressInto(std::span<const std::byte> source, std::span<std::byte> destination,
                                          Codec codec);

// Allocating versions of the two functions above. `level` 0 means the
// codec's default.
[[nodiscard]] Result<std::vector<std::byte>> compress(std::span<const std::byte> source, Codec codec,
                                                      int level = 0);
[[nodiscard]] Result<std::vector<std::byte>> decompress(std::span<const std::byte> source, Codec codec,
                                                        usize originalSize);

} // namespace vxe::io
