#pragma once

// Redundant state filtering for the OpenGL backend (Section 7.5).

#include <vxe/core/types.hpp>
#include <vxe/rhi/types.hpp>

#include <glad/gl.h>

#include <array>
#include <optional>
#include <span>

namespace vxe::rhi::gl {

// Remembers the fixed function state OpenGL currently has, so binding a
// pipeline only issues the GL calls for state that actually differs from
// the previous pipeline. OpenGL is a global state machine and every call
// costs driver time, so frames that switch between a few pipelines would
// otherwise repeat dozens of identical calls.
//
// Every value starts unknown (std::nullopt), so the first pipeline sets
// everything once. invalidate() returns to that state, for when something
// outside the RHI has changed GL state behind the cache's back.
//
// Thread safety: main thread only, like the device that owns it.
class StateCache {
public:
    void invalidate() noexcept;

    void useProgram(GLuint program);
    // Call when a program is deleted: OpenGL may hand the same name to a new
    // program, which must not be mistaken for the one already in use.
    void forgetProgram(GLuint program) noexcept;

    void applyRaster(const RasterState& raster);
    void applyDepth(const DepthState& depth);
    void applyBlend(std::span<const BlendState, kMaxColorAttachments> blend);

    // glClear only clears depth while depth writes are on, so render passes
    // call this before clearing even if the last pipeline had them off.
    void enableDepthWrites();

    // The number of state calls actually sent to OpenGL, for the overlay.
    [[nodiscard]] u64 stateChanges() const noexcept { return m_changes; }

private:
    // Sets `cached` to `value` and runs `send` (the GL call) only if they
    // differ, counting the call.
    template <typename T, typename Send>
    void set(std::optional<T>& cached, const T& value, Send&& send) {
        if (cached != value) {
            send();
            cached = value;
            ++m_changes;
        }
    }

    struct BlendSlot {
        std::optional<bool> enabled;
        std::optional<std::array<GLenum, 4>> factors; // srcColor, dstColor, srcAlpha, dstAlpha.
        std::optional<std::array<GLenum, 2>> ops;     // color, alpha.
    };

    std::optional<GLuint> m_program;
    std::optional<GLenum> m_polygonMode;
    std::optional<bool> m_cullEnabled;
    std::optional<GLenum> m_cullFace;
    std::optional<GLenum> m_frontFace;
    std::optional<bool> m_depthClamp;
    std::optional<bool> m_depthTest;
    std::optional<bool> m_depthWrite;
    std::optional<GLenum> m_depthFunc;
    std::array<BlendSlot, kMaxColorAttachments> m_blend{};
    u64 m_changes = 0;
};

} // namespace vxe::rhi::gl
