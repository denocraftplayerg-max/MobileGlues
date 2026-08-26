// MobileGlues - gl/drawing.cpp
// Copyright (c) 2025 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1
// SPDX-License-Identifier: LGPL-2.1-only

#include "drawing.h"
#include "restart.h"
#include "buffer.h"
#include "framebuffer.h"
#include "mg.h"
#include "texture.h"
#include "../egl/context.h"

#define DEBUG 0

GLuint bufSampelerProg;
GLuint bufSampelerLoc;
std::string bufSampelerName;

extern UnorderedMap<GLuint, bool> program_map_is_sampler_buffer_emulated;
UnorderedMap<GLuint, SamplerInfo> g_samplerCacheForSamplerBuffer;

namespace {

const GLint kBufferTextureUnit = 15;

struct resolved_program_t {
    GLuint program = 0;
    const SamplerInfo* source = nullptr;
    bool emulated = false;
    GLint locWidth = -1;
    GLint locHeight = -1;
    std::vector<GLint> samplers;
};

thread_local resolved_program_t g_resolved_program;

const resolved_program_t& resolve_program(GLuint program) {
    const auto emu = program_map_is_sampler_buffer_emulated.find(program);
    if (emu == program_map_is_sampler_buffer_emulated.end() || !emu->second) {
        g_resolved_program.program = program;
        g_resolved_program.source = nullptr;
        g_resolved_program.emulated = false;
        g_resolved_program.locWidth = -1;
        g_resolved_program.locHeight = -1;
        g_resolved_program.samplers.clear();
        return g_resolved_program;
    }

    auto it = g_samplerCacheForSamplerBuffer.find(program);
    if (it != g_samplerCacheForSamplerBuffer.end() && g_resolved_program.program == program &&
        g_resolved_program.source == &it->second) {
        return g_resolved_program;
    }

    const SamplerInfo* info = nullptr;
    if (it != g_samplerCacheForSamplerBuffer.end()) {
        info = &it->second;
    } else {
        SamplerInfo built{};
        built.locWidth = GLES.glGetUniformLocation(program, "u_BufferTexWidth");
        built.locHeight = GLES.glGetUniformLocation(program, "u_BufferTexHeight");
        if (built.locWidth != -1) {
            GLint numUniforms = 0;
            GLES.glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &numUniforms);
            for (GLint i = 0; i < numUniforms; ++i) {
                const GLsizei bufSize = 256;
                GLchar name[bufSize];
                GLsizei length = 0;
                GLint size = 0;
                GLenum type = 0;
                GLES.glGetActiveUniform(program, i, bufSize, &length, &size, &type, name);
                if (type == GL_SAMPLER_2D || type == GL_INT_SAMPLER_2D) {
                    built.samplers.push_back(GLES.glGetUniformLocation(program, name));
                }
            }
        }
        info = &(g_samplerCacheForSamplerBuffer[program] = std::move(built));
    }

    g_resolved_program.program = program;
    g_resolved_program.source = info;
    g_resolved_program.samplers.clear();

    if (info->locWidth == -1) {
        g_resolved_program.emulated = false;
        g_resolved_program.locWidth = -1;
        g_resolved_program.locHeight = -1;
        return g_resolved_program;
    }

    g_resolved_program.emulated = true;
    g_resolved_program.locWidth = info->locWidth;
    g_resolved_program.locHeight = info->locHeight;
    g_resolved_program.samplers = info->samplers;
    return g_resolved_program;
}

} // namespace

void setupBufferTextureUniforms(GLuint program) {
    LOG_D("setupBufferTextureUniforms, program: %d", program);
    const resolved_program_t& info = resolve_program(program);
    if (!info.emulated || info.samplers.empty()) return;

    GLuint texId = 0;
    const int prev_unit = mg_driver_active_texture_unit();
    GLES.glActiveTexture(GL_TEXTURE0 + kBufferTextureUnit);
    GLint queried = 0;
    GLES.glGetIntegerv(GL_TEXTURE_BINDING_2D, &queried);
    GLES.glActiveTexture(GL_TEXTURE0 + prev_unit);
    texId = static_cast<GLuint>(queried);

    if (texId == 0) return;

    const TextureObject* texObject = mgGetTexObjectByID(texId);
    if (!texObject) return;

    bool wrote_sampler = false;
    for (const GLint locSampler : info.samplers) {
        if (locSampler < 0) continue;
        GLES.glUniform1i(locSampler, kBufferTextureUnit);
        wrote_sampler = true;
    }
    if (!wrote_sampler) return;

    GLES.glUniform1i(info.locWidth, texObject->width);
    GLES.glUniform1i(info.locHeight, texObject->height);
}

void prepareForDraw() {
    LOG_D("prepareForDraw...")
    if (hardware->emulate_texture_buffer) {
        setupBufferTextureUniforms(gl_state->current_program);
    }
}

void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei primcount) {
    LOG()
    LOG_D("glDrawElementsInstanced, mode: %d, count: %d, type: %d, indices: %p, primcount: %d",
          mode, count, type, indices, primcount)
    prepareForDraw();
    if (mg_restart_needs_rewrite(type) && mg_draw_elements_restart(mode, count, type, indices, 0, primcount)) return;
    const bool restart_fixed = mg_restart_needs_driver_fixed(type);
    if (restart_fixed) GLES.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    GLES.glDrawElementsInstanced(mode, count, type, indices, primcount);
    if (restart_fixed) GLES.glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    CHECK_GL_ERROR
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    LOG()
    LOG_D("glDrawElements, mode: %d, count: %d, type: %d, indices: %p", mode, count, type, indices)
    prepareForDraw();
    if (mg_restart_needs_rewrite(type) && mg_draw_elements_restart(mode, count, type, indices, 0, -1)) return;
    const bool restart_fixed = mg_restart_needs_driver_fixed(type);
    if (restart_fixed) GLES.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    GLES.glDrawElements(mode, count, type, indices);
    if (restart_fixed) GLES.glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    CHECK_GL_ERROR
}

void glBindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access,
                        GLenum format) {
    LOG()
    LOG_D("glBindImageTexture, unit: %d, texture: %d, level: %d, layered: %d, layer: %d, access: %d, format: %d",
          unit, texture, level, layered, layer, access, format)
    GLES.glBindImageTexture(unit, texture, level, layered, layer, access, format);
    CHECK_GL_ERROR
}

void glUniform1i(GLint location, GLint v0) {
    LOG()
    LOG_D("glUniform1i, location: %d, v0: %d", location, v0)
    GLES.glUniform1i(location, v0);
    CHECK_GL_ERROR
}

void glDispatchCompute(GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z) {
    LOG()
    LOG_D("glDispatchCompute, num_groups_x: %d, num_groups_y: %d, num_groups_z: %d", num_groups_x, num_groups_y, num_groups_z)
    GLES.glDispatchCompute(num_groups_x, num_groups_y, num_groups_z);
    CHECK_GL_ERROR
}

void glMemoryBarrier(GLbitfield barriers) {
    LOG()
    LOG_D("glMemoryBarrier, barriers: %d", barriers)
    GLES.glMemoryBarrier(barriers);
    CHECK_GL_ERROR
}

namespace {

// ============================================================================
// GL_EXT_draw_elements_base_vertex - RESOLUCAO OTIMIZADA
// ============================================================================
typedef void(GLAPIENTRY* mg_pfn_draw_elements_base_vertex_ext)(GLenum, GLsizei, GLenum, const void*, GLint);
static mg_pfn_draw_elements_base_vertex_ext g_draw_elements_base_vertex_ext = nullptr;
static bool g_draw_elements_base_vertex_resolved = false;

static bool mg_draw_elements_base_vertex_available() {
    if (g_draw_elements_base_vertex_resolved) return g_draw_elements_base_vertex_ext != nullptr;
    g_draw_elements_base_vertex_resolved = true;
    if (!GLES.glGetStringi || !GLES.glGetIntegerv) return false;
    GLint count = 0;
    GLES.glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const GLubyte* s = GLES.glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
        if (s && (strcmp(reinterpret_cast<const char*>(s), "GL_EXT_draw_elements_base_vertex") == 0 ||
                  strcmp(reinterpret_cast<const char*>(s), "GL_OES_draw_elements_base_vertex") == 0)) {
            extern "C" void* gles;
            if (gles && gles != reinterpret_cast<void*>(~(uintptr_t)0)) {
                g_draw_elements_base_vertex_ext = reinterpret_cast<mg_pfn_draw_elements_base_vertex_ext>(
                    dlsym(gles, "glDrawElementsBaseVertexEXT"));
                if (!g_draw_elements_base_vertex_ext) {
                    g_draw_elements_base_vertex_ext = reinterpret_cast<mg_pfn_draw_elements_base_vertex_ext>(
                        dlsym(gles, "glDrawElementsBaseVertexOES"));
                }
            }
            break;
        }
    }
    return g_draw_elements_base_vertex_ext != nullptr;
}

// Scratch index buffer com pooling para evitar alloc por draw call
struct basevertex_pool_t {
    GLuint ibo = 0;
    size_t capacity = 0;
    unsigned long long owner_ctx_id = 0;
};

thread_local basevertex_pool_t g_basevertex_pool;

void basevertex_check_context() {
    const unsigned long long cur = g_current_ctx ? g_current_ctx->id : 0;
    if (cur == g_basevertex_pool.owner_ctx_id) return;
    g_basevertex_pool.ibo = 0;
    g_basevertex_pool.capacity = 0;
    g_basevertex_pool.owner_ctx_id = cur;
}

thread_local std::vector<GLuint> g_basevertex_staging;

void* basevertex_staging(size_t bytes) {
    const size_t need = (bytes + sizeof(GLuint) - 1) / sizeof(GLuint);
    if (g_basevertex_staging.size() < need) g_basevertex_staging.resize(need);
    return g_basevertex_staging.data();
}

} // namespace

// ============================================================================
// glDrawElementsBaseVertex OTIMIZADO
// ============================================================================

void glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex) {
    LOG()
    LOG_D("glDrawElementsBaseVertex, mode: %d, count: %d, type: %d, indices: %p, basevertex: %d",
          mode, count, type, indices, basevertex);
    prepareForDraw();

    if (mg_restart_needs_rewrite(type) && mg_draw_elements_restart(mode, count, type, indices, basevertex, -1)) return;
    const bool restart_fixed = mg_restart_needs_driver_fixed(type);
    if (restart_fixed) GLES.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    struct RestartGuard {
        bool on;
        ~RestartGuard() { if (on) GLES.glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX); }
    } restart_guard{restart_fixed};

    // PRIORIDADE 1: Usar extensao nativa GL_EXT_draw_elements_base_vertex
    if (mg_draw_elements_base_vertex_available() && g_draw_elements_base_vertex_ext) {
        g_draw_elements_base_vertex_ext(mode, count, type, indices, basevertex);
        CHECK_GL_ERROR
        return;
    }

    // PRIORIDADE 2: Usar funcao nativa do driver (GLES 3.2+)
    if (GLES.glDrawElementsBaseVertex) {
        GLES.glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
        CHECK_GL_ERROR
        return;
    }

    // FALLBACK: Emulacao via rebase de indices
    if (hardware->es_version < 320) {
        LOG_D("Emulating glDrawElementsBaseVertex")
        if (basevertex == 0) {
            GLES.glDrawElements(mode, count, type, indices);
            return;
        }
        if (count <= 0) return;

        size_t indexSize;
        switch (type) {
        case GL_UNSIGNED_INT:   indexSize = sizeof(GLuint); break;
        case GL_UNSIGNED_SHORT: indexSize = sizeof(GLushort); break;
        case GL_UNSIGNED_BYTE:  indexSize = sizeof(GLubyte); break;
        default: return;
        }

        const size_t bytes = static_cast<size_t>(count) * indexSize;
        const GLuint prevElementBuffer = mg_driver_bound_buffer(GL_ELEMENT_ARRAY_BUFFER);
        void* tempIndices = basevertex_staging(bytes);

        if (prevElementBuffer != 0) {
            GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prevElementBuffer);
            void* srcData = GLES.glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER,
                                      static_cast<GLintptr>(reinterpret_cast<uintptr_t>(indices)),
                                      static_cast<GLsizeiptr>(bytes), GL_MAP_READ_BIT);
            if (!srcData) return;
            memcpy(tempIndices, srcData, bytes);
            GLES.glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
        } else {
            if (!indices) return;
            memcpy(tempIndices, indices, bytes);
        }

        switch (type) {
        case GL_UNSIGNED_INT:
            for (GLsizei j = 0; j < count; ++j) ((GLuint*)tempIndices)[j] += basevertex;
            break;
        case GL_UNSIGNED_SHORT:
            for (GLsizei j = 0; j < count; ++j) ((GLushort*)tempIndices)[j] += basevertex;
            break;
        case GL_UNSIGNED_BYTE:
            for (GLsizei j = 0; j < count; ++j) ((GLubyte*)tempIndices)[j] += basevertex;
            break;
        }

        // Pool de IBO para evitar glGenBuffers/glDeleteBuffers por draw call
        basevertex_check_context();
        if (!g_basevertex_pool.ibo) {
            GLES.glGenBuffers(1, &g_basevertex_pool.ibo);
            g_basevertex_pool.capacity = 0;
        }

        GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_basevertex_pool.ibo);
        if (bytes > g_basevertex_pool.capacity) {
            g_basevertex_pool.capacity = bytes * 2;
            GLES.glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(g_basevertex_pool.capacity), nullptr, GL_STREAM_DRAW);
        }
        GLES.glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes), tempIndices);
        GLES.glDrawElements(mode, count, type, nullptr);
        GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prevElementBuffer);
        CHECK_GL_ERROR
    } else {
        GLES.glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
    }
    CHECK_GL_ERROR
}

#define DR_WARN_ONCE(...)                                                                                              \
    do {                                                                                                               \
        static bool mg_dr_warned = false;                                                                              \
        if (!mg_dr_warned) {                                                                                           \
            mg_dr_warned = true;                                                                                       \
            LOG_W_FORCE(__VA_ARGS__)                                                                                   \
        }                                                                                                              \
    } while (0)

namespace {
struct restart_guard_t {
    bool on;
    explicit restart_guard_t(GLenum type) : on(mg_restart_needs_driver_fixed(type)) {
        if (on) GLES.glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    }
    ~restart_guard_t() { if (on) GLES.glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX); }
    restart_guard_t(const restart_guard_t&) = delete;
    restart_guard_t& operator=(const restart_guard_t&) = delete;
};
} // namespace

void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
    LOG()
    LOG_D("glDrawRangeElements, mode: %d, start: %u, end: %u, count: %d, type: %d", mode, start, end, count, type)
    prepareForDraw();
    if (mg_restart_needs_rewrite(type) && mg_draw_elements_restart(mode, count, type, indices, 0, -1)) return;
    restart_guard_t guard(type);
    GLES.glDrawRangeElements(mode, start, end, count, type, indices);
    CHECK_GL_ERROR
}

void glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                   const void* indices, GLint basevertex) {
    LOG()
    LOG_D("glDrawRangeElementsBaseVertex, mode: %d, count: %d, type: %d, basevertex: %d", mode, count, type, basevertex)
    prepareForDraw();
    if (mg_restart_needs_rewrite(type) && mg_draw_elements_restart(mode, count, type, indices, basevertex, -1)) return;
    restart_guard_t guard(type);
    if (GLES.glDrawRangeElementsBaseVertex) {
        GLES.glDrawRangeElementsBaseVertex(mode, start, end, count, type, indices, basevertex);
    } else {
        glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
    }
    CHECK_GL_ERROR
}

void glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                       GLsizei instancecount, GLint basevertex) {
    LOG()
    LOG_D("glDrawElementsInstancedBaseVertex, mode: %d, count: %d, type: %d, instancecount: %d, basevertex: %d",
          mode, count, type, instancecount, basevertex)
    prepareForDraw();
    if (mg_restart_needs_rewrite(type) &&
        mg_draw_elements_restart(mode, count, type, indices, basevertex, instancecount))
        return;
    restart_guard_t guard(type);
    if (GLES.glDrawElementsInstancedBaseVertex) {
        GLES.glDrawElementsInstancedBaseVertex(mode, count, type, indices, instancecount, basevertex);
    } else if (basevertex == 0) {
        GLES.glDrawElementsInstanced(mode, count, type, indices, instancecount);
    } else {
        DR_WARN_ONCE("glDrawElementsInstancedBaseVertex: no base vertex support on this context, drawing without it");
        GLES.glDrawElementsInstanced(mode, count, type, indices, instancecount);
    }
    CHECK_GL_ERROR
}

void glDrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                       GLuint baseinstance) {
    LOG()
    LOG_D("glDrawArraysInstancedBaseInstance, mode: %d, first: %d, count: %d, instancecount: %d, baseinstance: %u",
          mode, first, count, instancecount, baseinstance)
    if (baseinstance != 0) {
        DR_WARN_ONCE("glDrawArraysInstancedBaseInstance: baseinstance %u ignored, GLES has no base instance", baseinstance);
    }
    prepareForDraw();
    GLES.glDrawArraysInstanced(mode, first, count, instancecount);
    CHECK_GL_ERROR
}

void glDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                         GLsizei instancecount, GLuint baseinstance) {
    LOG()
    LOG_D("glDrawElementsInstancedBaseInstance, mode: %d, count: %d, type: %d, instancecount: %d, baseinstance: %u",
          mode, count, type, instancecount, baseinstance)
    if (baseinstance != 0) {
        DR_WARN_ONCE("glDrawElementsInstancedBaseInstance: baseinstance %u ignored, GLES has no base instance", baseinstance);
    }
    glDrawElementsInstanced(mode, count, type, indices, instancecount);
}

void glDrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                   GLsizei instancecount, GLint basevertex, GLuint baseinstance) {
    LOG()
    LOG_D("glDrawElementsInstancedBaseVertexBaseInstance, mode: %d, count: %d, basevertex: %d, baseinstance: %u",
          mode, count, basevertex, baseinstance)
    if (baseinstance != 0) {
        DR_WARN_ONCE("glDrawElementsInstancedBaseVertexBaseInstance: baseinstance %u ignored, GLES has no base instance", baseinstance);
    }
    glDrawElementsInstancedBaseVertex(mode, count, type, indices, instancecount, basevertex);
}
