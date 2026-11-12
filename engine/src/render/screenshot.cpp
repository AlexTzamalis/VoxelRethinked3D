#include "vxe/render/screenshot.hpp"

// stb_image_write makes the PNG; its implementation lives in this file only.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <stb_image_write.h>
#ifdef __clang__
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstring>

namespace vxe::render {
namespace {

void appendBytes(void* context, void* data, int size) {
    auto& out = *static_cast<std::vector<std::byte>*>(context);
    const auto* bytes = static_cast<const std::byte*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

} // namespace

Result<std::vector<std::byte>> encodePng(const Screenshot& shot) {
    if (shot.width == 0 || shot.height == 0 || shot.rgba.size() != usize{shot.width} * shot.height * 4) {
        return Error(ErrorCode::InvalidArgument, "A screenshot needs width x height RGBA pixels");
    }
    std::vector<std::byte> opaque = shot.rgba;
    for (usize i = 3; i < opaque.size(); i += 4) {
        opaque[i] = std::byte{255};
    }
    std::vector<std::byte> png;
    const int written =
        stbi_write_png_to_func(appendBytes, &png, static_cast<int>(shot.width), static_cast<int>(shot.height),
                               4, opaque.data(), static_cast<int>(shot.width * 4));
    if (written == 0) {
        return Error(ErrorCode::IoError, "PNG encoding failed");
    }
    return png;
}

} // namespace vxe::render
