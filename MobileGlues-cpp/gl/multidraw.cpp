// MobileGlues - gl/multidraw.cpp
// Copyright (c) 2025 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1
// SPDX-License-Identifier: LGPL-2.1-only

#include "multidraw.h"
#include "../config/settings.h"
#include "buffer.h"
#include "enable.h"
#include "restart.h"
#include "../egl/context.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include <cstring>

#define DEBUG 0

void prepareForDraw();

#define MD_WARN_ONCE(...)                                                                                              \
    do {                                                                                                               \
        static bool mg_md_warned = false;                                                                              \
        if (!mg_md_warned) {                                                                                           \
            mg_md_warned = true;                                                                                       \
            LOG_W_FORCE(__VA_ARGS__)                                                                                   \
        }                                                                                                              \
    } while (0)

static GLenum mg_md_check() { return GLES.glGetError(); }
static void mg_md_drain() {
    for (int i = 0; i < 16 && GLES.glGetError() != GL_NO_ERROR; ++i) {}
}

static inline GLsizei mg_index_size(GLenum type) {
    switch (type) {
    case GL_UNSIGNED_BYTE:  return 1;
    case GL_UNSIGNED_SHORT: return 2;
    case GL_UNSIGNED_INT:   return 4;
    default: return 0;
    }
}

static void mg_rebase_indices_to_u32(GLuint* dst, const void* src, GLsizei count, GLenum type, GLint basevertex,
                                     bool restart_enabled, GLuint sentinel) {
    const GLuint bv = static_cast<GLuint>(basevertex);
    #define MG_REBASE_LOOP(SRCTYPE)                                                                                    \
        do {                                                                                                           \
            const SRCTYPE* s = static_cast<const SRCTYPE*>(src);                                                       \
            if (restart_enabled) {                                                                                     \
                for (GLsizei j = 0; j < count; ++j)                                                                    \
                    dst[j] = (static_cast<GLuint>(s[j]) == sentinel) ? 0xFFFFFFFFu : (static_cast<GLuint>(s[j]) + bv);   \
            } else {                                                                                                   \
                for (GLsizei j = 0; j < count; ++j)                                                                    \
                    dst[j] = static_cast<GLuint>(s[j]) + bv;                                                           \
            }                                                                                                          \
        } while (0)

    switch (type) {
    case GL_UNSIGNED_INT:   MG_REBASE_LOOP(GLuint); break;
    case GL_UNSIGNED_SHORT: MG_REBASE_LOOP(GLushort); break;
    case GL_UNSIGNED_BYTE:  MG_REBASE_LOOP(GLubyte); break;
    default: break;
    }
    #undef MG_REBASE_LOOP
}

static GLsizei mg_verts_per_primitive(GLenum mode) {
    switch (mode) {
    case GL_POINTS:           return 1;
    case GL_LINES:            return 2;
    case GL_TRIANGLES:        return 3;
    case GL_LINES_ADJACENCY:  return 4;
    case GL_TRIANGLES_ADJACENCY: return 6;
    default: return 0;
    }
}

static bool is_strip_like_mode(GLenum mode) {
    return mode == GL_LINE_STRIP || mode == GL_LINE_LOOP || mode == GL_TRIANGLE_STRIP ||
           mode == GL_TRIANGLE_FAN || mode == GL_QUAD_STRIP || mode == GL_POLYGON;
}

extern "C" void* gles;

typedef void(GLAPIENTRY* mg_pfn_multi_draw_arrays_ext)(GLenum, const GLint*, const GLsizei*, GLsizei);
typedef void(GLAPIENTRY* mg_pfn_multi_draw_elements_ext)(GLenum, const GLsizei*, GLenum, const void* const*, GLsizei);

static mg_pfn_multi_draw_arrays_ext  g_mda_ext = nullptr;
static mg_pfn_multi_draw_elements_ext g_mde_ext = nullptr;

static bool mg_gles_has_extension(const char* name) {
    if (!GLES.glGetStringi || !GLES.glGetIntegerv) return false;
    GLint count = 0;
    GLES.glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const GLubyte* s = GLES.glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
        if (s && strcmp(reinterpret_cast<const char*>(s), name) == 0) return true;
    }
    return false;
}

bool mg_multi_draw_arrays_ext_available() {
    static bool resolved = false;
    static bool available = false;
    if (!resolved) {
        resolved = true;
        const bool ext   = mg_gles_has_extension("GL_EXT_multi_draw_arrays");
        const bool angle = mg_gles_has_extension("GL_ANGLE_multi_draw");
        const bool real_handle = gles != nullptr && gles != reinterpret_cast<void*>(~(uintptr_t)0);
        if (real_handle && (ext || angle)) {
            const char* arrays_name  = ext ? "glMultiDrawArraysEXT"  : "glMultiDrawArraysANGLE";
            const char* elements_name = ext ? "glMultiDrawElementsEXT" : "glMultiDrawElementsANGLE";
            g_mda_ext = reinterpret_cast<mg_pfn_multi_draw_arrays_ext>(dlsym(gles, arrays_name));
            g_mde_ext = reinterpret_cast<mg_pfn_multi_draw_elements_ext>(dlsym(gles, elements_name));
            if (g_mda_ext && g_mde_ext) {
                available = true;
                LOG_D("GL_EXT_multi_draw_arrays / GL_ANGLE_multi_draw resolved OK");
            }
        }
    }
    return available;
}

// ============================================================================
// GL_EXT_buffer_storage - RESOLUCAO
// ============================================================================
typedef void(GLAPIENTRY* mg_pfn_buffer_storage_ext)(GLenum, GLsizeiptr, const void*, GLbitfield);
static mg_pfn_buffer_storage_ext g_buffer_storage_ext = nullptr;
static bool g_buffer_storage_available = false;

bool mg_buffer_storage_ext_available() {
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (mg_gles_has_extension("GL_EXT_buffer_storage")) {
            const bool real_handle = gles != nullptr && gles != reinterpret_cast<void*>(~(uintptr_t)0);
            if (real_handle) {
                g_buffer_storage_ext = reinterpret_cast<mg_pfn_buffer_storage_ext>(dlsym(gles, "glBufferStorageEXT"));
                g_buffer_storage_available = (g_buffer_storage_ext != nullptr);
            }
        }
    }
    return g_buffer_storage_available;
}

// ============================================================================
// GL_EXT_draw_elements_base_vertex - RESOLUCAO
// ============================================================================
typedef void(GLAPIENTRY* mg_pfn_draw_elements_base_vertex_ext)(GLenum, GLsizei, GLenum, const void*, GLint);
static mg_pfn_draw_elements_base_vertex_ext g_draw_elements_base_vertex_ext = nullptr;
static bool g_draw_elements_base_vertex_available = false;

bool mg_draw_elements_base_vertex_ext_available() {
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (mg_gles_has_extension("GL_EXT_draw_elements_base_vertex")) {
            const bool real_handle = gles != nullptr && gles != reinterpret_cast<void*>(~(uintptr_t)0);
            if (real_handle) {
                g_draw_elements_base_vertex_ext = reinterpret_cast<mg_pfn_draw_elements_base_vertex_ext>(
                    dlsym(gles, "glDrawElementsBaseVertexEXT"));
                g_draw_elements_base_vertex_available = (g_draw_elements_base_vertex_ext != nullptr);
            }
        }
    }
    return g_draw_elements_base_vertex_available;
}

// ============================================================================
// MULTI-DRAW INDIRECT EMULADO (sem extensao nativa)
// Tecnica: Fusao de indices + single draw call
// ============================================================================

static thread_local GLuint g_fused_ibo = 0;
static thread_local size_t g_fused_ibo_capacity = 0;

static void mg_ensure_fused_ibo(size_t needed) {
    if (needed <= g_fused_ibo_capacity) return;
    if (g_fused_ibo) GLES.glDeleteBuffers(1, &g_fused_ibo);
    GLES.glGenBuffers(1, &g_fused_ibo);
    g_fused_ibo_capacity = needed * 2;
    GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_fused_ibo);
    GLES.glBufferData(GL_ELEMENT_ARRAY_BUFFER, g_fused_ibo_capacity, nullptr, GL_STREAM_DRAW);
}

// Emula MultiDrawElementsBaseVertex fundindo indices em um unico buffer
void mg_glMultiDrawElementsBaseVertex_indirect_emulated(
    GLenum mode, GLsizei* counts, GLenum type,
    const void* const* indices, GLsizei primcount,
    const GLint* basevertex) {

    if (primcount <= 0) return;
    if (primcount == 1) {
        glDrawElementsBaseVertex(mode, counts[0], type, indices[0], basevertex ? basevertex[0] : 0);
        return;
    }

    GLsizei index_size = mg_index_size(type);
    if (index_size == 0) return;

    // Verificar se todos os basevertex sao iguais
    bool all_same_basevertex = true;
    GLint common_basevertex = basevertex ? basevertex[0] : 0;
    if (basevertex) {
        for (GLsizei i = 1; i < primcount; ++i) {
            if (basevertex[i] != common_basevertex) { all_same_basevertex = false; break; }
        }
    }

    // Calcular tamanho total
    size_t total_indices = 0;
    for (GLsizei i = 0; i < primcount; ++i) total_indices += counts[i];
    size_t total_bytes = total_indices * index_size;

    // Estrategia: fundir todos os indices em um buffer contiguo
    mg_ensure_fused_ibo(total_bytes);
    GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_fused_ibo);

    GLubyte* dst = (GLubyte*)GLES.glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, total_bytes,
                                                     GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    if (!dst) {
        // Fallback: loop individual
        for (GLsizei i = 0; i < primcount; ++i) {
            if (counts[i] <= 0) continue;
            if (basevertex && basevertex[i] != 0 && g_draw_elements_base_vertex_ext) {
                g_draw_elements_base_vertex_ext(mode, counts[i], type, indices[i], basevertex[i]);
            } else {
                GLES.glDrawElements(mode, counts[i], type, indices[i]);
            }
        }
        return;
    }

    // Copiar indices (com rebase se basevertex diferentes)
    size_t offset = 0;
    if (all_same_basevertex && common_basevertex == 0) {
        // Sem rebase necessario
        for (GLsizei i = 0; i < primcount; ++i) {
            size_t nbytes = (size_t)counts[i] * index_size;
            memcpy(dst + offset, indices[i], nbytes);
            offset += nbytes;
        }
    } else if (type == GL_UNSIGNED_INT) {
        for (GLsizei i = 0; i < primcount; ++i) {
            const GLuint* src = (const GLuint*)indices[i];
            GLuint bv = basevertex ? (GLuint)basevertex[i] : 0;
            for (GLsizei j = 0; j < counts[i]; ++j) {
                ((GLuint*)dst)[offset / 4 + j] = src[j] + bv;
            }
            offset += (size_t)counts[i] * index_size;
        }
    } else if (type == GL_UNSIGNED_SHORT) {
        for (GLsizei i = 0; i < primcount; ++i) {
            const GLushort* src = (const GLushort*)indices[i];
            GLushort bv = basevertex ? (GLushort)basevertex[i] : 0;
            for (GLsizei j = 0; j < counts[i]; ++j) {
                ((GLushort*)dst)[offset / 2 + j] = src[j] + bv;
            }
            offset += (size_t)counts[i] * index_size;
        }
    } else {
        for (GLsizei i = 0; i < primcount; ++i) {
            const GLubyte* src = (const GLubyte*)indices[i];
            GLubyte bv = basevertex ? (GLubyte)basevertex[i] : 0;
            for (GLsizei j = 0; j < counts[i]; ++j) {
                dst[offset + j] = src[j] + bv;
            }
            offset += (size_t)counts[i] * index_size;
        }
    }

    GLES.glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
    GLES.glDrawElements(mode, (GLsizei)total_indices, type, nullptr);
}

// ============================================================================
// MULTI-DRAW ARRAYS OTIMIZADO
// ============================================================================

static thread_local GLuint g_fused_vbo = 0;
static thread_local size_t g_fused_vbo_capacity = 0;

void mg_glMultiDrawArrays_optimized(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
    if (drawcount <= 0) return;

    // Tentar usar extensao nativa primeiro
    if (mg_multi_draw_arrays_ext_available() && g_mda_ext) {
        g_mda_ext(mode, first, count, drawcount);
        return;
    }

    // Fallback: fundir ranges contiguos
    if (drawcount == 1) {
        GLES.glDrawArrays(mode, first[0], count[0]);
        return;
    }

    // Verificar se todos sao contiguos
    bool contiguous = true;
    for (GLsizei i = 1; i < drawcount; ++i) {
        if (first[i] != first[i-1] + count[i-1]) {
            contiguous = false;
            break;
        }
    }

    if (contiguous) {
        GLsizei total = first[drawcount-1] + count[drawcount-1] - first[0];
        GLES.glDrawArrays(mode, first[0], total);
    } else {
        // Loop com batching em grupos de 8
        const GLsizei BATCH = 8;
        for (GLsizei i = 0; i < drawcount; i += BATCH) {
            GLsizei end = (i + BATCH < drawcount) ? i + BATCH : drawcount;
            for (GLsizei j = i; j < end; ++j) {
                if (count[j] > 0) GLES.glDrawArrays(mode, first[j], count[j]);
            }
        }
    }
}

// ============================================================================
// MULTI-DRAW ELEMENTS OTIMIZADO
// ============================================================================
void mg_glMultiDrawElements_optimized(GLenum mode, const GLsizei* count, GLenum type,
                                        const void* const* indices, GLsizei primcount) {
    if (primcount <= 0) return;

    // Tentar extensao nativa
    if (mg_multi_draw_arrays_ext_available() && g_mde_ext) {
        g_mde_ext(mode, count, type, indices, primcount);
        return;
    }

    // Usar emulacao indirect
    mg_glMultiDrawElementsBaseVertex_indirect_emulated(mode, const_cast<GLsizei*>(count), type, indices, primcount, nullptr);
}

// ============================================================================
// MULTI-DRAW ELEMENTS BASE VERTEX OTIMIZADO
// ============================================================================
void mg_glMultiDrawElementsBaseVertex_optimized(GLenum mode, GLsizei* counts, GLenum type,
                                                const void* const* indices, GLsizei primcount,
                                                const GLint* basevertex) {
    if (primcount <= 0) return;

    // Tentar extensao nativa para multi-draw
    if (mg_multi_draw_arrays_ext_available() && g_mde_ext && primcount > 1) {
        bool same_base = true;
        for (GLsizei i = 1; i < primcount; ++i) {
            if (basevertex[i] != basevertex[0]) { same_base = false; break; }
        }
        if (same_base && basevertex[0] == 0) {
            g_mde_ext(mode, counts, type, indices, primcount);
            return;
        }
    }

    // Tentar EXT_draw_elements_base_vertex individualmente
    if (g_draw_elements_base_vertex_ext && primcount <= 4) {
        for (GLsizei i = 0; i < primcount; ++i) {
            if (counts[i] > 0) {
                g_draw_elements_base_vertex_ext(mode, counts[i], type, indices[i], basevertex[i]);
            }
        }
        return;
    }

    // Emulacao indirect para batches grandes
    if (primcount >= 4) {
        mg_glMultiDrawElementsBaseVertex_indirect_emulated(mode, counts, type, indices, primcount, basevertex);
    } else {
        for (GLsizei i = 0; i < primcount; ++i) {
            if (counts[i] > 0) glDrawElementsBaseVertex(mode, counts[i], type, indices[i], basevertex[i]);
        }
    }
}

// ============================================================================
// ENTRY POINTS PUBLICOS (exportados)
// ============================================================================

void glMultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
    LOG()
    LOG_D("glMultiDrawArrays, mode: %d, drawcount: %d", mode, drawcount)
    prepareForDraw();
    mg_glMultiDrawArrays_optimized(mode, first, count, drawcount);
    CHECK_GL_ERROR
}

void glMultiDrawElements(GLenum mode, const GLsizei* count, GLenum type,
                         const void* const* indices, GLsizei primcount) {
    LOG()
    LOG_D("glMultiDrawElements, mode: %d, primcount: %d", mode, primcount)
    prepareForDraw();
    mg_glMultiDrawElements_optimized(mode, count, type, indices, primcount);
    CHECK_GL_ERROR
}

void glMultiDrawElementsBaseVertex(GLenum mode, GLsizei* counts, GLenum type,
                                   const void* const* indices, GLsizei primcount,
                                   const GLint* basevertex) {
    LOG()
    LOG_D("glMultiDrawElementsBaseVertex, mode: %d, primcount: %d", mode, primcount)
    prepareForDraw();
    mg_glMultiDrawElementsBaseVertex_optimized(mode, counts, type, indices, primcount, basevertex);
    CHECK_GL_ERROR
}

// ============================================================================
// MULTI-DRAW INDIRECT (com extensao ou emulado)
// ============================================================================

void glMultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {
    LOG()
    LOG_D("glMultiDrawArraysIndirect, mode: %d, drawcount: %d", mode, drawcount)
    prepareForDraw();

    if (GLES.glMultiDrawArraysIndirect) {
        GLES.glMultiDrawArraysIndirect(mode, indirect, drawcount, stride);
    } else if (drawcount > 0) {
        const GLsizei cmd_stride = stride ? stride : sizeof(draw_arrays_indirect_command_t);
        const GLubyte* cmd_ptr = static_cast<const GLubyte*>(indirect);

        const GLsizei MAX_BATCH = 64;
        GLint first_batch[MAX_BATCH];
        GLsizei count_batch[MAX_BATCH];
        GLsizei batch_count = 0;

        for (GLsizei i = 0; i < drawcount; ++i) {
            const draw_arrays_indirect_command_t* cmd =
                reinterpret_cast<const draw_arrays_indirect_command_t*>(cmd_ptr + i * cmd_stride);
            if (cmd->count == 0) continue;

            first_batch[batch_count] = static_cast<GLint>(cmd->first);
            count_batch[batch_count] = static_cast<GLsizei>(cmd->count);
            batch_count++;

            if (batch_count >= MAX_BATCH || i == drawcount - 1) {
                mg_glMultiDrawArrays_optimized(mode, first_batch, count_batch, batch_count);
                batch_count = 0;
            }
        }
        if (batch_count > 0) {
            mg_glMultiDrawArrays_optimized(mode, first_batch, count_batch, batch_count);
        }
    }
    CHECK_GL_ERROR
}

void glMultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect,
                                 GLsizei drawcount, GLsizei stride) {
    LOG()
    LOG_D("glMultiDrawElementsIndirect, mode: %d, type: %d, drawcount: %d", mode, type, drawcount)
    prepareForDraw();

    if (GLES.glMultiDrawElementsIndirect) {
        GLES.glMultiDrawElementsIndirect(mode, type, indirect, drawcount, stride);
    } else if (drawcount > 0) {
        const GLsizei cmd_stride = stride ? stride : sizeof(draw_elements_indirect_command_t);
        const GLubyte* cmd_ptr = static_cast<const GLubyte*>(indirect);

        const GLsizei MAX_BATCH = 64;
        GLsizei count_batch[MAX_BATCH];
        const void* indices_batch[MAX_BATCH];
        GLint basevertex_batch[MAX_BATCH];
        GLsizei batch_count = 0;
        GLsizei index_size = mg_index_size(type);

        for (GLsizei i = 0; i < drawcount; ++i) {
            const draw_elements_indirect_command_t* cmd =
                reinterpret_cast<const draw_elements_indirect_command_t*>(cmd_ptr + i * cmd_stride);
            if (cmd->count == 0) continue;

            count_batch[batch_count] = static_cast<GLsizei>(cmd->count);
            indices_batch[batch_count] = reinterpret_cast<const void*>(static_cast<uintptr_t>(cmd->firstIndex) * index_size);
            basevertex_batch[batch_count] = cmd->baseVertex;
            batch_count++;

            if (batch_count >= MAX_BATCH || i == drawcount - 1) {
                mg_glMultiDrawElementsBaseVertex_optimized(mode, count_batch, type, indices_batch, batch_count, basevertex_batch);
                batch_count = 0;
            }
        }
        if (batch_count > 0) {
            mg_glMultiDrawElementsBaseVertex_optimized(mode, count_batch, type, indices_batch, batch_count, basevertex_batch);
        }
    }
    CHECK_GL_ERROR
}

// ============================================================================
// MULTI-DRAW INDIRECT COUNT
// ============================================================================

void glMultiDrawArraysIndirectCount(GLenum mode, const void* indirect, GLintptr drawcount,
                                    GLsizei maxdrawcount, GLsizei stride) {
    LOG()
    LOG_D("glMultiDrawArraysIndirectCount, mode: %d, maxdrawcount: %d", mode, maxdrawcount)
    prepareForDraw();

    if (GLES.glMultiDrawArraysIndirectCount) {
        GLES.glMultiDrawArraysIndirectCount(mode, indirect, drawcount, maxdrawcount, stride);
    } else {
        GLint actual_count = 0;
        if (drawcount >= 0) {
            const GLuint param_buf = mg_driver_bound_buffer(GL_PARAMETER_BUFFER);
            if (param_buf) {
                GLES.glBindBuffer(GL_COPY_WRITE_BUFFER, param_buf);
                void* mapped = GLES.glMapBufferRange(GL_COPY_WRITE_BUFFER, drawcount, sizeof(GLint), GL_MAP_READ_BIT);
                if (mapped) {
                    actual_count = *static_cast<const GLint*>(mapped);
                    GLES.glUnmapBuffer(GL_COPY_WRITE_BUFFER);
                }
            }
        }
        if (actual_count > maxdrawcount) actual_count = maxdrawcount;
        if (actual_count > 0) {
            glMultiDrawArraysIndirect(mode, indirect, actual_count, stride);
        }
    }
    CHECK_GL_ERROR
}

void glMultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect,
                                      GLintptr drawcount, GLsizei maxdrawcount, GLsizei stride) {
    LOG()
    LOG_D("glMultiDrawElementsIndirectCount, mode: %d, maxdrawcount: %d", mode, maxdrawcount)
    prepareForDraw();

    if (GLES.glMultiDrawElementsIndirectCount) {
        GLES.glMultiDrawElementsIndirectCount(mode, type, indirect, drawcount, maxdrawcount, stride);
    } else {
        GLint actual_count = 0;
        if (drawcount >= 0) {
            const GLuint param_buf = mg_driver_bound_buffer(GL_PARAMETER_BUFFER);
            if (param_buf) {
                GLES.glBindBuffer(GL_COPY_WRITE_BUFFER, param_buf);
                void* mapped = GLES.glMapBufferRange(GL_COPY_WRITE_BUFFER, drawcount, sizeof(GLint), GL_MAP_READ_BIT);
                if (mapped) {
                    actual_count = *static_cast<const GLint*>(mapped);
                    GLES.glUnmapBuffer(GL_COPY_WRITE_BUFFER);
                }
            }
        }
        if (actual_count > maxdrawcount) actual_count = maxdrawcount;
        if (actual_count > 0) {
            glMultiDrawElementsIndirect(mode, type, indirect, actual_count, stride);
        }
    }
    CHECK_GL_ERROR
}

// ============================================================================
// BACKWARDS COMPATIBILITY WRAPPERS
// ============================================================================

void mg_glMultiDrawElementsBaseVertex_indirect(GLenum mode, GLsizei* counts, GLenum type,
                                               const void* const* indices, GLsizei primcount,
                                               const GLint* basevertex) {
    mg_glMultiDrawElementsBaseVertex_optimized(mode, counts, type, indices, primcount, basevertex);
}

void mg_glMultiDrawElementsBaseVertex_multiindirect(GLenum mode, GLsizei* counts, GLenum type,
                                                    const void* const* indices, GLsizei primcount,
                                                    const GLint* basevertex) {
    mg_glMultiDrawElementsBaseVertex_indirect_emulated(mode, counts, type, indices, primcount, basevertex);
}

void mg_glMultiDrawElementsBaseVertex_basevertex(GLenum mode, GLsizei* counts, GLenum type,
                                                 const void* const* indices, GLsizei primcount,
                                                 const GLint* basevertex) {
    mg_glMultiDrawElementsBaseVertex_optimized(mode, counts, type, indices, primcount, basevertex);
}

void mg_glMultiDrawElementsBaseVertex_drawelements(GLenum mode, GLsizei* counts, GLenum type,
                                                   const void* const* indices, GLsizei primcount,
                                                   const GLint* basevertex) {
    for (GLsizei i = 0; i < primcount; ++i) {
        if (counts[i] > 0) glDrawElementsBaseVertex(mode, counts[i], type, indices[i], basevertex[i]);
    }
}

void mg_glMultiDrawElementsBaseVertex_compute(GLenum mode, GLsizei* counts, GLenum type,
                                              const void* const* indices, GLsizei primcount,
                                              const GLint* basevertex) {
    mg_glMultiDrawElementsBaseVertex_indirect_emulated(mode, counts, type, indices, primcount, basevertex);
}

void mg_glMultiDrawElementsBaseVertex_multibasevertex(GLenum mode, GLsizei* counts, GLenum type,
                                                      const void* const* indices, GLsizei primcount,
                                                      const GLint* basevertex) {
    mg_glMultiDrawElementsBaseVertex_optimized(mode, counts, type, indices, primcount, basevertex);
}

void mg_glMultiDrawElements_indirect(GLenum mode, const GLsizei* count, GLenum type,
                                     const void* const* indices, GLsizei primcount) {
    mg_glMultiDrawElements_optimized(mode, count, type, indices, primcount);
}

void mg_glMultiDrawElements_multiindirect(GLenum mode, const GLsizei* count, GLenum type,
                                          const void* const* indices, GLsizei primcount) {
    mg_glMultiDrawElements_optimized(mode, count, type, indices, primcount);
}

void mg_glMultiDrawElements_basevertex(GLenum mode, const GLsizei* count, GLenum type,
                                       const void* const* indices, GLsizei primcount) {
    mg_glMultiDrawElements_optimized(mode, count, type, indices, primcount);
}

void mg_glMultiDrawElements_drawelements(GLenum mode, const GLsizei* count, GLenum type,
                                         const void* const* indices, GLsizei primcount) {
    for (GLsizei i = 0; i < primcount; ++i) {
        if (count[i] > 0) GLES.glDrawElements(mode, count[i], type, indices[i]);
    }
}

void mg_glMultiDrawElements_compute(GLenum mode, const GLsizei* count, GLenum type,
                                    const void* const* indices, GLsizei primcount) {
    mg_glMultiDrawElements_optimized(mode, count, type, indices, primcount);
}

void mg_glMultiDrawElements_multibasevertex(GLenum mode, const GLsizei* count, GLenum type,
                                            const void* const* indices, GLsizei primcount) {
    mg_glMultiDrawElements_optimized(mode, count, type, indices, primcount);
}

void mg_glMultiDrawElements_multiarrays(GLenum mode, const GLsizei* count, GLenum type,
                                         const void* const* indices, GLsizei primcount) {
    mg_glMultiDrawElements_optimized(mode, count, type, indices, primcount);
}

void mg_glMultiDrawArrays_unroll(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
    for (GLsizei i = 0; i < drawcount; ++i) {
        if (count[i] > 0) GLES.glDrawArrays(mode, first[i], count[i]);
    }
}

void mg_glMultiDrawArrays_multiarrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
    mg_glMultiDrawArrays_optimized(mode, first, count, drawcount);
}

void mg_glMultiDrawArrays_multiindirect(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
    mg_glMultiDrawArrays_optimized(mode, first, count, drawcount);
}
