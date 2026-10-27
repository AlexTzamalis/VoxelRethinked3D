#include "gl_state.hpp"

#include "gl_formats.hpp"

namespace vxe::rhi::gl {
namespace {

void setCapability(GLenum capability, bool enabled) {
    if (enabled) {
        glEnable(capability);
    } else {
        glDisable(capability);
    }
}

} // namespace

void StateCache::invalidate() noexcept {
    m_program.reset();
    m_polygonMode.reset();
    m_cullEnabled.reset();
    m_cullFace.reset();
    m_frontFace.reset();
    m_depthClamp.reset();
    m_depthTest.reset();
    m_depthWrite.reset();
    m_depthFunc.reset();
    m_blend = {};
}

void StateCache::useProgram(GLuint program) {
    set(m_program, program, [program] { glUseProgram(program); });
}

void StateCache::forgetProgram(GLuint program) noexcept {
    if (m_program == program) {
        m_program.reset();
    }
}

void StateCache::applyRaster(const RasterState& raster) {
    const GLenum polygon = polygonMode(raster.polygonMode);
    set(m_polygonMode, polygon, [polygon] { glPolygonMode(GL_FRONT_AND_BACK, polygon); });

    const bool cull = raster.cullMode != CullMode::None;
    set(m_cullEnabled, cull, [cull] { setCapability(GL_CULL_FACE, cull); });
    if (cull) {
        // The face only matters while culling is on, so it is not touched otherwise.
        const GLenum face = raster.cullMode == CullMode::Front ? GL_FRONT : GL_BACK;
        set(m_cullFace, face, [face] { glCullFace(face); });
    }
    const GLenum front = frontFace(raster.frontFace);
    set(m_frontFace, front, [front] { glFrontFace(front); });
    set(m_depthClamp, raster.depthClamp, [&raster] { setCapability(GL_DEPTH_CLAMP, raster.depthClamp); });
}

void StateCache::applyDepth(const DepthState& depth) {
    set(m_depthTest, depth.testEnabled, [&depth] { setCapability(GL_DEPTH_TEST, depth.testEnabled); });
    set(m_depthWrite, depth.writeEnabled, [&depth] { glDepthMask(depth.writeEnabled ? GL_TRUE : GL_FALSE); });
    if (depth.testEnabled) {
        const GLenum func = compareOp(depth.compare);
        set(m_depthFunc, func, [func] { glDepthFunc(func); });
    }
}

void StateCache::applyBlend(std::span<const BlendState, kMaxColorAttachments> blend) {
    for (u32 i = 0; i < kMaxColorAttachments; ++i) {
        const BlendState& state = blend[i];
        BlendSlot& slot = m_blend[i];
        set(slot.enabled, state.enabled, [i, &state] {
            if (state.enabled) {
                glEnablei(GL_BLEND, i);
            } else {
                glDisablei(GL_BLEND, i);
            }
        });
        if (!state.enabled) {
            continue; // Factors and equations only matter while blending is on.
        }
        const std::array<GLenum, 4> factors{blendFactor(state.srcColor), blendFactor(state.dstColor),
                                            blendFactor(state.srcAlpha), blendFactor(state.dstAlpha)};
        set(slot.factors, factors,
            [i, &factors] { glBlendFuncSeparatei(i, factors[0], factors[1], factors[2], factors[3]); });
        const std::array<GLenum, 2> ops{blendOp(state.colorOp), blendOp(state.alphaOp)};
        set(slot.ops, ops, [i, &ops] { glBlendEquationSeparatei(i, ops[0], ops[1]); });
    }
}

void StateCache::enableDepthWrites() {
    set(m_depthWrite, true, [] { glDepthMask(GL_TRUE); });
}

} // namespace vxe::rhi::gl
